#include "ViscositySolver.h"

#include <Eigen/Sparse>
#include <iostream>

#include "ComputeWeights.h"
#include "LevelSet.h"

namespace FluidSim3D::SimTools
{
using SolveReal = double;
using Vector = Eigen::VectorXd;

void ViscositySolver(float dt, const LevelSet& surface, VectorGrid<float>& velocity, const LevelSet& solidSurface,
                     const VectorGrid<float>& solidVelocity, const ScalarGrid<float>& viscosity)
{
    // For efficiency sake, this should only take in velocity on a staggered grid
    // that matches the center sampled surface and collision
    assert(surface.isGridMatched(solidSurface));
    assert(surface.isGridMatched(viscosity));
    assert(velocity.isGridMatched(solidVelocity));

    for (int axis : {0, 1, 2})
    {
        Vec3i faceSize = velocity.size(axis);
        --faceSize[axis];

        assert(faceSize == surface.size());
    }

    int volumeSamples = 3;

    ScalarGrid<float> centerVolumes(surface.xform(), surface.size(), 0, ScalarGridSettings::SampleType::CENTER);
    computeSupersampleVolumes(centerVolumes, surface, 3);

    VectorGrid<float> edgeVolumes(surface.xform(), surface.size(), 0, VectorGridSettings::SampleType::EDGE);
    for (int axis : {0, 1, 2}) computeSupersampleVolumes(edgeVolumes.grid(axis), surface, 3);

    VectorGrid<float> faceVolumes = computeSupersampledFaceVolumes(surface, 3);

    enum class MaterialLabels
    {
        SOLID_FACE,
        LIQUID_FACE,
        AIR_FACE
    };

    VectorGrid<MaterialLabels> materialFaceLabels(surface.xform(), surface.size(), MaterialLabels::AIR_FACE,
                                                  VectorGridSettings::SampleType::STAGGERED);

    // Set material labels for each grid face. We assume faces along the simulation boundary
    // are solid.

    for (int faceAxis : {0, 1, 2})
    {
        tbb::parallel_for(
            tbb::blocked_range<int>(0, materialFaceLabels.grid(faceAxis).voxelCount(), tbbLightGrainSize),
            [&](const tbb::blocked_range<int>& range) {
                for (int faceIndex = range.begin(); faceIndex != range.end(); ++faceIndex)
                {
                    Vec3i face = materialFaceLabels.grid(faceAxis).unflatten(faceIndex);

                    if (face[faceAxis] == 0 || face[faceAxis] == materialFaceLabels.size(faceAxis)[faceAxis] - 1)
                        continue;

                    bool isFaceInSolve = false;

                    for (int direction : {0, 1})
                    {
                        Vec3i cell = faceToCell(face, faceAxis, direction);
                        if (centerVolumes(cell) > 0) isFaceInSolve = true;
                    }

                    if (!isFaceInSolve)
                    {
                        for (int edgeAxis : {0, 1, 2})
                        {
                            if (edgeAxis == faceAxis) continue;

                            for (int direction : {0, 1})
                            {
                                Vec3i edge = faceToEdge(face, faceAxis, edgeAxis, direction);

                                if (edgeVolumes(edge, edgeAxis) > 0) isFaceInSolve = true;
                            }
                        }
                    }

                    if (isFaceInSolve)
                    {
                        if (solidSurface.interp(materialFaceLabels.indexToWorld(Vec3f(face), faceAxis)) <= 0.)
                            materialFaceLabels(face, faceAxis) = MaterialLabels::SOLID_FACE;
                        else
                            materialFaceLabels(face, faceAxis) = MaterialLabels::LIQUID_FACE;
                    }
                }
            });
    }

    int liquidDOFCount = 0;

    constexpr int UNLABELLED_CELL = -1;

    VectorGrid<int> liquidFaceIndices(surface.xform(), surface.size(), UNLABELLED_CELL,
                                      VectorGridSettings::SampleType::STAGGERED);

    for (int axis : {0, 1, 2})
    {
        forEachVoxelRange(Vec3i(0), materialFaceLabels.size(axis), [&](const Vec3i& face) {
            if (materialFaceLabels(face, axis) == MaterialLabels::LIQUID_FACE)
                liquidFaceIndices(face, axis) = liquidDOFCount++;
        });
    }

    SolveReal discreteScalar = dt / sqr(surface.dx());

    // Pre-scale all the control volumes with coefficients to reduce
    // redundant operations when building the linear system.

    tbb::parallel_for(tbb::blocked_range<int>(0, centerVolumes.voxelCount(), tbbLightGrainSize),
                      [&](const tbb::blocked_range<int>& range) {
                          for (int cellIndex = range.begin(); cellIndex != range.end(); ++cellIndex)
                          {
                              Vec3i cell = centerVolumes.unflatten(cellIndex);

                              if (centerVolumes(cell) > 0) centerVolumes(cell) *= 2. * discreteScalar * viscosity(cell);
                          }
                      });

    for (int edgeAxis : {0, 1, 2})
    {
        tbb::parallel_for(tbb::blocked_range<int>(0, edgeVolumes.grid(edgeAxis).voxelCount(), tbbLightGrainSize),
                          [&](const tbb::blocked_range<int>& range) {
                              for (int edgeIndex = range.begin(); edgeIndex != range.end(); ++edgeIndex)
                              {
                                  Vec3i edge = edgeVolumes.grid(edgeAxis).unflatten(edgeIndex);

                                  if (edgeVolumes(edge, edgeAxis) > 0)
                                      edgeVolumes(edge, edgeAxis) *=
                                          discreteScalar *
                                          viscosity.interp(edgeVolumes.indexToWorld(Vec3f(edge), edgeAxis));
                              }
                          });
    }

    std::vector<Eigen::Triplet<SolveReal>> sparseElements;
    Vector initialGuessVector = Vector::Zero(liquidDOFCount);
    Vector rhsVector = Vector::Zero(liquidDOFCount);

    {
        tbb::enumerable_thread_specific<std::vector<Eigen::Triplet<SolveReal>>> parallelSparseElements;

        for (int faceAxis : {0, 1, 2})
        {
            tbb::parallel_for(
                tbb::blocked_range<int>(0, materialFaceLabels.grid(faceAxis).voxelCount(), tbbLightGrainSize),
                [&](const tbb::blocked_range<int>& range) {
                    auto& localSparseElements = parallelSparseElements.local();

                    for (int faceIndex = range.begin(); faceIndex != range.end(); ++faceIndex)
                    {
                        Vec3i face = materialFaceLabels.grid(faceAxis).unflatten(faceIndex);

                        int liquidFaceIndex = liquidFaceIndices(face, faceAxis);

                        if (liquidFaceIndex >= 0)
                        {
                            assert(materialFaceLabels(face, faceAxis) == MaterialLabels::LIQUID_FACE);

                            // Use old velocity as an initial guess since we're solving for a new
                            // velocity field with viscous forces applied to the old velocity field.
                            initialGuessVector(liquidFaceIndex) = velocity(face, faceAxis);

                            // Build RHS with volume weights
                            SolveReal localFaceVolume = faceVolumes(face, faceAxis);

                            rhsVector(liquidFaceIndex) = localFaceVolume * velocity(face, faceAxis);

                            // Add volume weight to diagonal
                            SolveReal diagonal = localFaceVolume;

                            // Build cell centered stress terms
                            for (int divergenceDirection : {0, 1})
                            {
                                Vec3i cell = faceToCell(face, faceAxis, divergenceDirection);

                                assert(cell[faceAxis] >= 0 && cell[faceAxis] < centerVolumes.size()[faceAxis]);

                                SolveReal divergenceSign = (divergenceDirection == 0) ? -1 : 1;

                                if (centerVolumes(cell) > 0)
                                {
                                    for (int gradientDirection : {0, 1})
                                    {
                                        Vec3i adjacentFace = cellToFace(cell, faceAxis, gradientDirection);

                                        SolveReal gradientSign = (gradientDirection == 0) ? -1. : 1.;

                                        SolveReal coefficient = divergenceSign * gradientSign * centerVolumes(cell);

                                        int adjacentFaceIndex = liquidFaceIndices(adjacentFace, faceAxis);
                                        if (adjacentFaceIndex >= 0)
                                        {
                                            if (adjacentFaceIndex == liquidFaceIndex)
                                                diagonal -= coefficient;
                                            else
                                                localSparseElements.emplace_back(liquidFaceIndex, adjacentFaceIndex,
                                                                                 -coefficient);
                                        }
                                        else if (materialFaceLabels(adjacentFace, faceAxis) ==
                                                 MaterialLabels::SOLID_FACE)
                                            rhsVector(liquidFaceIndex) +=
                                                coefficient * solidVelocity(adjacentFace, faceAxis);
                                        else
                                            assert(materialFaceLabels(adjacentFace, faceAxis) ==
                                                   MaterialLabels::AIR_FACE);
                                    }
                                }
                            }

                            for (int edgeAxis : {0, 1, 2})
                            {
                                if (edgeAxis == faceAxis) continue;

                                for (int divergenceDirection : {0, 1})
                                {
                                    Vec3i edge = faceToEdge(face, faceAxis, edgeAxis, divergenceDirection);

                                    if (edgeVolumes(edge, edgeAxis) > 0)
                                    {
                                        SolveReal divergenceSign = (divergenceDirection == 0) ? -1 : 1;

                                        for (int gradientAxis : {0, 1, 2})
                                        {
                                            if (gradientAxis == edgeAxis) continue;

                                            int gradientFaceAxis = 3 - gradientAxis - edgeAxis;

                                            for (int gradientDirection : {0, 1})
                                            {
                                                SolveReal gradientSign = (gradientDirection == 0) ? -1 : 1;

                                                Vec3i localGradientFace =
                                                    edgeToFace(edge, edgeAxis, gradientFaceAxis, gradientDirection);

                                                int gradientFaceIndex =
                                                    liquidFaceIndices(localGradientFace, gradientFaceAxis);

                                                SolveReal coefficient =
                                                    divergenceSign * gradientSign * edgeVolumes(edge, edgeAxis);
                                                if (gradientFaceIndex >= 0)
                                                {
                                                    if (gradientFaceIndex == liquidFaceIndex)
                                                        diagonal -= coefficient;
                                                    else
                                                        localSparseElements.emplace_back(
                                                            liquidFaceIndex, gradientFaceIndex, -coefficient);
                                                }
                                                else if (materialFaceLabels(localGradientFace, gradientFaceAxis) ==
                                                         MaterialLabels::SOLID_FACE)
                                                    rhsVector(liquidFaceIndex) +=
                                                        coefficient *
                                                        solidVelocity(localGradientFace, gradientFaceAxis);
                                                else
                                                    assert(materialFaceLabels(localGradientFace, gradientFaceAxis) ==
                                                           MaterialLabels::AIR_FACE);
                                            }
                                        }
                                    }
                                }
                            }

                            localSparseElements.emplace_back(liquidFaceIndex, liquidFaceIndex, diagonal);
                        }
                        else
                            assert(materialFaceLabels(face, faceAxis) != MaterialLabels::LIQUID_FACE);
                    }
                });
        }

        mergeLocalThreadVectors(sparseElements, parallelSparseElements);
    }

    Eigen::SparseMatrix<SolveReal> sparseMatrix(liquidDOFCount, liquidDOFCount);
    sparseMatrix.setFromTriplets(sparseElements.begin(), sparseElements.end());

    Eigen::ConjugateGradient<Eigen::SparseMatrix<SolveReal>, Eigen::Upper | Eigen::Lower> solver;
    solver.compute(sparseMatrix);
    solver.setTolerance(1E-3);

    if (solver.info() != Eigen::Success)
    {
        std::cout << "   Solver failed to build" << std::endl;
        return;
    }

    Vector solutionVector = solver.solveWithGuess(rhsVector, initialGuessVector);

    if (solver.info() != Eigen::Success)
    {
        std::cout << "   Solver failed to converge" << std::endl;
        return;
    }
    else
    {
        std::cout << "    Solver iterations:     " << solver.iterations() << std::endl;
        std::cout << "    Solver error: " << solver.error() << std::endl;
    }

    for (int faceAxis : {0, 1, 2})
    {
        tbb::parallel_for(tbb::blocked_range<int>(0, materialFaceLabels.grid(faceAxis).voxelCount(), tbbLightGrainSize),
                          [&](const tbb::blocked_range<int>& range) {
                              for (int faceIndex = range.begin(); faceIndex != range.end(); ++faceIndex)
                              {
                                  Vec3i face = materialFaceLabels.grid(faceAxis).unflatten(faceIndex);

                                  int liquidFaceIndex = liquidFaceIndices(face, faceAxis);
                                  if (liquidFaceIndex >= 0)
                                  {
                                      assert(materialFaceLabels(face, faceAxis) == MaterialLabels::LIQUID_FACE);
                                      velocity(face, faceAxis) = solutionVector(liquidFaceIndex);
                                  }
                              }
                          });
    }
}

}  // namespace FluidSim3D::SimTools