#include "HignnModel.hpp"

void HignnModel::PostCheckDot(DeviceDoubleMatrix u, DeviceDoubleMatrix f) {
  if (mMPIRank == 0)
    std::cout << "start of PostCheckDot" << std::endl;

  DeviceDoubleMatrix uPostCheck("uPostCheck", u.extent(0), u.extent(1));

  Kokkos::parallel_for(
      Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, u.extent(0)),
      KOKKOS_LAMBDA(const int i) {
        uPostCheck(i, 0) = 0.0;
        uPostCheck(i, 1) = 0.0;
        uPostCheck(i, 2) = 0.0;
      });
  Kokkos::fence();

  double queryDuration = 0;
  double dotDuration = 0;

  const std::size_t totalLeafNodeSize = mLeafNodeList.size();

  std::size_t leafNodeStart = 0, leafNodeEnd;
  for (unsigned int i = 0; i < (unsigned int)mMPIRank; i++) {
    std::size_t rankLeafNodeSize =
        totalLeafNodeSize / mMPISize + (i < totalLeafNodeSize % mMPISize);
    leafNodeStart += rankLeafNodeSize;
  }
  leafNodeEnd =
      leafNodeStart + totalLeafNodeSize / mMPISize +
      ((unsigned int)mMPIRank < totalLeafNodeSize % (unsigned int)mMPISize);
  leafNodeEnd = std::min(leafNodeEnd, totalLeafNodeSize);

  const std::size_t leafNodeSize = leafNodeEnd - leafNodeStart;

  const std::size_t maxWorkSize = std::min<std::size_t>(
      leafNodeSize, mMaxRelativeCoord / (mBlockSize * mBlockSize));
  int workSize = maxWorkSize;

  std::size_t totalNumQuery = 0;
  std::size_t totalNumIter = 0;

  DeviceFloatVector relativeCoordPool(
      "relativeCoordPool", maxWorkSize * mBlockSize * mBlockSize * 3);

  DeviceIntVector workingNode("workingNode", maxWorkSize);
  DeviceIntVector workingNodeOffset("workingNodeOffset", maxWorkSize);
  DeviceIntVector workingNodeCpy("workingNodeCpy", maxWorkSize);
  DeviceIntVector workingFlag("workingFlag", maxWorkSize);
  DeviceIntVector workingNodeCol("workingNodeCol", maxWorkSize);

  DeviceIntVector relativeCoordSize("relativeCoordSize", maxWorkSize);
  DeviceIntVector relativeCoordOffset("relativeCoordOffset", maxWorkSize);

  DeviceIndexVector nodeOffset("nodeOffset", leafNodeSize);

  auto &mCoord = *mCoordPtr;
  auto &mClusterTree = *mClusterTreePtr;

  DeviceIntVector mLeafNode("mLeafNode", totalLeafNodeSize);

  DeviceIntVector::HostMirror hostLeafNode =
      Kokkos::create_mirror_view(mLeafNode);

  for (size_t i = 0; i < totalLeafNodeSize; i++) {
    hostLeafNode(i) = mLeafNodeList[i];
  }
  Kokkos::deep_copy(mLeafNode, hostLeafNode);

  Kokkos::parallel_for(
      Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, leafNodeSize),
      KOKKOS_LAMBDA(const std::size_t i) { nodeOffset(i) = 0; });

  Kokkos::parallel_for(
      Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
      KOKKOS_LAMBDA(const std::size_t i) {
        workingNode(i) = i;
        workingFlag(i) = 1;
      });

  int workingFlagSum = workSize;
  while (workSize > 0) {
    totalNumIter++;
    int totalCoord = 0;
    Kokkos::parallel_reduce(
        Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
        KOKKOS_LAMBDA(const std::size_t i, int &tSum) {
          const int rank = i;
          const int node = workingNode(rank);
          const int nodeI = mLeafNode(node + leafNodeStart);
          const int nodeJ = mLeafNode(nodeOffset(node));

          const int indexIStart = mClusterTree(nodeI, 2);
          const int indexIEnd = mClusterTree(nodeI, 3);
          const int indexJStart = mClusterTree(nodeJ, 2);
          const int indexJEnd = mClusterTree(nodeJ, 3);

          const int workSizeI = indexIEnd - indexIStart;
          const int workSizeJ = indexJEnd - indexJStart;

          relativeCoordSize(rank) = workSizeI * workSizeJ;

          tSum += workSizeI * workSizeJ;
        },
        Kokkos::Sum<int>(totalCoord));
    Kokkos::fence();

    totalNumQuery += totalCoord;
    Kokkos::parallel_for(
        Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
        KOKKOS_LAMBDA(const int rank) {
          relativeCoordOffset(rank) = 0;
          for (int i = 0; i < rank; i++) {
            relativeCoordOffset(rank) += relativeCoordSize(i);
          }
        });
    Kokkos::fence();

    // calculate the relative coordinates
    Kokkos::parallel_for(
        Kokkos::TeamPolicy<Kokkos::DefaultExecutionSpace>(workSize,
                                                          Kokkos::AUTO()),
        KOKKOS_LAMBDA(
            const Kokkos::TeamPolicy<Kokkos::DefaultExecutionSpace>::member_type
                &teamMember) {
          const int rank = teamMember.league_rank();
          const int node = workingNode(rank);
          const int nodeI = mLeafNode(node + leafNodeStart);
          const int nodeJ = mLeafNode(nodeOffset(node));
          const int relativeOffset = relativeCoordOffset(rank);

          const int indexIStart = mClusterTree(nodeI, 2);
          const int indexIEnd = mClusterTree(nodeI, 3);
          const int indexJStart = mClusterTree(nodeJ, 2);
          const int indexJEnd = mClusterTree(nodeJ, 3);

          const int workSizeI = indexIEnd - indexIStart;
          const int workSizeJ = indexJEnd - indexJStart;

          Kokkos::parallel_for(
              Kokkos::TeamThreadRange(teamMember, workSizeI * workSizeJ),
              [&](const int i) {
                int j = i / workSizeJ;
                int k = i % workSizeJ;

                const int index = relativeOffset + j * workSizeJ + k;
                for (int l = 0; l < 3; l++) {
                  relativeCoordPool(3 * index + l) =
                      mCoord(indexJStart + k, l) - mCoord(indexIStart + j, l);
                }
              });
        });
    Kokkos::fence();

    // do inference
#if USE_GPU
    auto options = torch::TensorOptions()
                       .dtype(torch::kFloat32)
                       .device(torch::kCUDA, mCudaDevice)
                       .requires_grad(false);
#else
    auto options = torch::TensorOptions()
                       .dtype(torch::kFloat32)
                       .device(torch::kCPU)
                       .requires_grad(false);
#endif

    torch::Tensor relativeCoordTensor =
        torch::from_blob(relativeCoordPool.data(), {totalCoord, 3}, options);
    std::vector<c10::IValue> inputs;
    inputs.push_back(relativeCoordTensor);

    std::chrono::steady_clock::time_point begin =
        std::chrono::steady_clock::now();

    auto resultTensor = mTwoBodyModel.forward(inputs).toTensor();

    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now();
    queryDuration +=
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
            .count();

    begin = std::chrono::steady_clock::now();

    auto dataPtr = resultTensor.data_ptr<float>();

    Kokkos::parallel_for(
        Kokkos::TeamPolicy<Kokkos::DefaultExecutionSpace>(workSize,
                                                          Kokkos::AUTO()),
        KOKKOS_LAMBDA(
            const Kokkos::TeamPolicy<Kokkos::DefaultExecutionSpace>::member_type
                &teamMember) {
          const int rank = teamMember.league_rank();
          const int node = workingNode(rank);
          const int nodeI = mLeafNode(node + leafNodeStart);
          const int nodeJ = mLeafNode(nodeOffset(node));
          const int relativeOffset = relativeCoordOffset(rank);

          const int indexIStart = mClusterTree(nodeI, 2);
          const int indexIEnd = mClusterTree(nodeI, 3);
          const int indexJStart = mClusterTree(nodeJ, 2);
          const int indexJEnd = mClusterTree(nodeJ, 3);

          const int workSizeI = indexIEnd - indexIStart;
          const int workSizeJ = indexJEnd - indexJStart;

          Kokkos::parallel_for(
              Kokkos::TeamThreadRange(teamMember, workSizeI * workSizeJ),
              [&](const std::size_t index) {
                const std::size_t j = index / workSizeJ;
                const std::size_t k = index % workSizeJ;
                for (int row = 0; row < 3; row++) {
                  double sum = 0.0;
                  for (int col = 0; col < 3; col++)
                    sum +=
                        0.5 *
                        (dataPtr[9 * (relativeOffset + index) + row * 3 + col] +
                         dataPtr[9 * (relativeOffset + index) + col * 3 +
                                 row]) *
                        f(indexJStart + k, col);
                  Kokkos::atomic_add(&uPostCheck(indexIStart + j, row), sum);
                }
              });
        });
    Kokkos::fence();

    end = std::chrono::steady_clock::now();
    dotDuration +=
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
            .count();

    // if (totalNumIter <= 4) {
    //   DeviceDoubleMatrix::HostMirror hostU =
    //       Kokkos::create_mirror_view(uPostCheck);
    //   Kokkos::deep_copy(hostU, uPostCheck);

    //   DeviceIntVector::HostMirror hostWorkingNode =
    //       Kokkos::create_mirror_view(workingNode);
    //   Kokkos::deep_copy(hostWorkingNode, workingNode);

    //   DeviceIndexVector::HostMirror hostNodeOffset =
    //       Kokkos::create_mirror_view(nodeOffset);
    //   Kokkos::deep_copy(hostNodeOffset, nodeOffset);

    //   std::cout << hostU(0, 0) << " " << hostU(0, 1) << " " << hostU(0, 2)
    //             << std::endl;
    //   std::cout << "working node: " << hostWorkingNode(0) << " "
    //             << hostNodeOffset(0) << std::endl;

    //   DeviceFloatVector::HostMirror hostRelativeCoord =
    //       Kokkos::create_mirror_view(relativeCoordPool);
    //   Kokkos::deep_copy(hostRelativeCoord, relativeCoordPool);

    //   DeviceFloatMatrix queryResult("queryResult", totalCoord, 9);

    //   Kokkos::parallel_for(
    //       Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, totalCoord),
    //       KOKKOS_LAMBDA(const int i) {
    //         for (int j = 0; j < 9; j++) {
    //           queryResult(i, j) = dataPtr[9 * i + j];
    //         }
    //       });
    //   Kokkos::fence();

    //   DeviceFloatMatrix::HostMirror hostQueryResult =
    //       Kokkos::create_mirror_view(queryResult);
    //   Kokkos::deep_copy(hostQueryResult, queryResult);

    //   for (int i = 0; i < totalCoord; i++)
    //     std::cout << "  coord(" << i << "): " << hostRelativeCoord(3 * i) <<
    //     " "
    //               << hostRelativeCoord(3 * i + 1) << " "
    //               << hostRelativeCoord(3 * i + 2) << " query result(" << i
    //               << "): " << hostQueryResult(i, 0) << " "
    //               << hostQueryResult(i, 1) << " " << hostQueryResult(i, 2)
    //               << " " << hostQueryResult(i, 3) << " "
    //               << hostQueryResult(i, 4) << " " << hostQueryResult(i, 5)
    //               << " " << hostQueryResult(i, 6) << " "
    //               << hostQueryResult(i, 7) << " " << hostQueryResult(i, 8)
    //               << std::endl;
    // }

    // post processing
    Kokkos::parallel_for(
        Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
        KOKKOS_LAMBDA(const int rank) { workingFlag(rank) = 1; });
    Kokkos::fence();

    Kokkos::parallel_for(
        Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
        KOKKOS_LAMBDA(const int rank) {
          nodeOffset(workingNode(rank))++;
          if (nodeOffset(workingNode(rank)) == totalLeafNodeSize) {
            workingNode(rank) += maxWorkSize;
          }

          if (workingNode(rank) >= (int)leafNodeSize) {
            workingFlag(rank) = 0;
          }
        });
    Kokkos::fence();

    Kokkos::parallel_reduce(
        Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
        KOKKOS_LAMBDA(const std::size_t i, int &tSum) {
          tSum += workingFlag(i);
        },
        Kokkos::Sum<int>(workingFlagSum));
    Kokkos::fence();

    if (workSize > workingFlagSum) {
      // copy the working node to working node cpy and shrink the work size
      Kokkos::parallel_for(
          Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
          KOKKOS_LAMBDA(const int rank) {
            workingNodeCpy(rank) = workingNode(rank);
            int counter = rank + 1;
            if (rank < workingFlagSum)
              for (int i = 0; i < workSize; i++) {
                if (workingFlag(i) == 1) {
                  counter--;
                }
                if (counter == 0) {
                  workingNodeOffset(rank) = i;
                  break;
                }
              }
          });
      Kokkos::fence();

      workSize = workingFlagSum;

      Kokkos::parallel_for(
          Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, workSize),
          KOKKOS_LAMBDA(const int rank) {
            workingNode(rank) = workingNodeCpy(workingNodeOffset(rank));
          });
      Kokkos::fence();
    }
  }

  DeviceDoubleMatrix::HostMirror hostU = Kokkos::create_mirror_view(uPostCheck);
  Kokkos::deep_copy(hostU, uPostCheck);

  MPI_Allreduce(MPI_IN_PLACE, hostU.data(), hostU.extent(0) * hostU.extent(1),
                MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  Kokkos::deep_copy(uPostCheck, hostU);

  double uNorm, diffNorm;
  Kokkos::parallel_reduce(
      Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, u.extent(0)),
      KOKKOS_LAMBDA(const int i, double &tSum) {
        tSum += uPostCheck(i, 0) * uPostCheck(i, 0) +
                uPostCheck(i, 1) * uPostCheck(i, 1) +
                uPostCheck(i, 2) * uPostCheck(i, 2);
      },
      Kokkos::Sum<double>(uNorm));
  Kokkos::fence();

  Kokkos::parallel_reduce(
      Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>(0, u.extent(0)),
      KOKKOS_LAMBDA(const int i, double &tSum) {
        tSum += pow(u(i, 0) - uPostCheck(i, 0), 2) +
                pow(u(i, 1) - uPostCheck(i, 1), 2) +
                pow(u(i, 2) - uPostCheck(i, 2), 2);
      },
      Kokkos::Sum<double>(diffNorm));
  Kokkos::fence();

  if (mMPIRank == 0) {
    std::cout << "query duration: " << queryDuration / 1e6 << "s" << std::endl;
    std::cout << "dot product duration: " << dotDuration / 1e6 << "s"
              << std::endl;
    std::cout << "post check result: " << sqrt(diffNorm / uNorm) << std::endl;
  }

  // if (sqrt(diffNorm / uNorm) > 1e-3) {
  //   // get f index
  //   DeviceDoubleMatrix::HostMirror hostF = Kokkos::create_mirror_view(f);
  //   Kokkos::deep_copy(hostF, f);

  //   size_t idx1 = 0;
  //   for (size_t i = 0; i < u.extent(0); i++) {
  //     double norm = 0.0;
  //     for (int j = 0; j < 3; j++) {
  //       norm += hostF(i, j) * hostF(i, j);
  //     }

  //     if (norm > 1e-3) {
  //       idx1 = i;
  //       break;
  //     }
  //   }

  //   DeviceFloatMatrix::HostMirror hostCoord =
  //       Kokkos::create_mirror_view(mCoord);
  //   Kokkos::deep_copy(hostCoord, mCoord);

  //   std::cout << "idx: " << idx1 << " coord: " << hostCoord(idx1, 0) << " "
  //             << hostCoord(idx1, 1) << " " << hostCoord(idx1, 2) <<
  //             std::endl;
  //   std::cout << "f: " << hostF(idx1, 0) << " " << hostF(idx1, 1) << " "
  //             << hostF(idx1, 2) << std::endl;

  //   for (size_t i = 0; i < hostCoord.extent(0); i++) {
  //     std::cout << "  idx: " << i << " coord: " << hostCoord(i, 0) << " "
  //               << hostCoord(i, 1) << " " << hostCoord(i, 2) << std::endl;
  //   }

  //   DeviceDoubleMatrix::HostMirror hostU = Kokkos::create_mirror_view(u);
  //   Kokkos::deep_copy(hostU, u);

  //   DeviceDoubleMatrix::HostMirror hostUPostCheck =
  //       Kokkos::create_mirror_view(uPostCheck);
  //   Kokkos::deep_copy(hostUPostCheck, uPostCheck);

  //   size_t idx2 = 0;
  //   for (size_t i = 0; i < u.extent(0); i++) {
  //     double norm = 0.0;
  //     for (int j = 0; j < 3; j++) {
  //       norm += pow(hostU(i, j) - hostUPostCheck(i, j), 2);
  //     }

  //     if (norm > 1e-3) {
  //       idx2 = i;
  //       break;
  //     }
  //   }

  //   std::cout << "idx: " << idx2 << " u: " << hostU(idx2, 0) << " "
  //             << hostU(idx2, 1) << " " << hostU(idx2, 2) << std::endl;
  //   std::cout << "uPostCheck: " << hostUPostCheck(idx2, 0) << " "
  //             << hostUPostCheck(idx2, 1) << " " << hostUPostCheck(idx2, 2)
  //             << std::endl;
  //   std::cout << "f: " << hostF(idx2, 0) << " " << hostF(idx2, 1) << " "
  //             << hostF(idx2, 2) << std::endl;
  //   std::cout << "u: " << hostU(idx1, 0) << " " << hostU(idx1, 1) << " "
  //             << hostU(idx1, 2) << std::endl;

  //   DeviceIndexMatrix::HostMirror clusterTreeHost =
  //       Kokkos::create_mirror_view(mClusterTree);
  //   Kokkos::deep_copy(clusterTreeHost, mClusterTree);

  //   for (size_t i = 0; i < mLeafNodeList.size(); i++) {
  //     std::cout << "  leaf node: " << mLeafNodeList[i] << " "
  //               << clusterTreeHost(mLeafNodeList[i], 2) << " "
  //               << clusterTreeHost(mLeafNodeList[i], 3) << std::endl;
  //   }

  //   auto &mCloseMatI = *mCloseMatIPtr;
  //   auto &mCloseMatJ = *mCloseMatJPtr;

  //   DeviceIndexVector::HostMirror closeMatIHost =
  //       Kokkos::create_mirror_view(mCloseMatI);
  //   DeviceIndexVector::HostMirror closeMatJHost =
  //       Kokkos::create_mirror_view(mCloseMatJ);
  //   Kokkos::deep_copy(closeMatIHost, mCloseMatI);
  //   Kokkos::deep_copy(closeMatJHost, mCloseMatJ);

  //   for (size_t i = 0; i < closeMatIHost.extent(0); i++) {
  //     std::cout << "  close mat: " << i << " " << closeMatIHost(i) << " "
  //               << closeMatJHost(i) << std::endl;
  //   }

  //   for (size_t i = 0; i < u.extent(0); i++) {
  //     std::cout << "idx: " << i << std::endl;
  //     std::cout << "  u:          " << hostU(i, 0) << " " << hostU(i, 1) << "
  //     "
  //               << hostU(i, 2) << std::endl;
  //     std::cout << "  uPostCheck: " << hostUPostCheck(i, 0) << " "
  //               << hostUPostCheck(i, 1) << " " << hostUPostCheck(i, 2)
  //               << std::endl;
  //   }
  // }
}