#include "HignnModel.hpp"

void HignnModel::CloseFarCheck() {
  std::chrono::high_resolution_clock::time_point t1 =
      std::chrono::high_resolution_clock::now();

  if (mMPIRank == 0)
    std::cout << "start of CloseFarCheck" << std::endl;

  mLeafNodeList.clear();

  auto &mClusterTree = *mClusterTreeMirrorPtr;

  HostFloatMatrix mAux;
  Kokkos::resize(mAux, mClusterTree.extent(0), 6);

  // init mAux
  for (int i = 0; i < mAux.extent(0); i++)
    mAux(i, 0) = -std::numeric_limits<float>::max();

  // go over all nodes, calculate aux based on the tree structure.
  std::stack<std::size_t> workStack;
  workStack.push(0);
  while (workStack.size() != 0) {
    auto node = workStack.top();
    if (mClusterTree(node, 0) != 0) {
      // check if child nodes have calculated aux.
      if (mAux(mClusterTree(node, 0), 0) ==
          -std::numeric_limits<float>::max()) {
        workStack.push(mClusterTree(node, 0));
        workStack.push(mClusterTree(node, 1));
        continue;
      } else {
        mAux(node, 0) = std::min(mAux(mClusterTree(node, 0), 0),
                                 mAux(mClusterTree(node, 1), 0));
        mAux(node, 1) = std::max(mAux(mClusterTree(node, 0), 1),
                                 mAux(mClusterTree(node, 1), 1));
        mAux(node, 2) = std::min(mAux(mClusterTree(node, 0), 2),
                                 mAux(mClusterTree(node, 1), 2));
        mAux(node, 3) = std::max(mAux(mClusterTree(node, 0), 3),
                                 mAux(mClusterTree(node, 1), 3));
        mAux(node, 4) = std::min(mAux(mClusterTree(node, 0), 4),
                                 mAux(mClusterTree(node, 1), 4));
        mAux(node, 5) = std::max(mAux(mClusterTree(node, 0), 5),
                                 mAux(mClusterTree(node, 1), 5));

        workStack.pop();
      }
    } else {
      std::vector<float> aux(6);
      ComputeAux(mClusterTree(node, 2), mClusterTree(node, 3), aux);

      mAux(node, 0) = aux[0];
      mAux(node, 1) = aux[1];
      mAux(node, 2) = aux[2];
      mAux(node, 3) = aux[3];
      mAux(node, 4) = aux[4];
      mAux(node, 5) = aux[5];

      workStack.pop();
    }
  }

  // estimate far and close pair size based on aux.
  // close far check
  std::vector<std::vector<int>> farMat(mClusterTree.extent(0));
  std::vector<std::vector<int>> closeMat(mClusterTree.extent(0));

  std::size_t total_entry = 0;

  closeMat[0].push_back(0);
  for (int i = 0; i < mClusterTree.extent(0); i++) {
    auto node = i;

    if (mClusterTree(node, 0) != 0) {
      std::vector<int> childCloseMat;

      for (int j = 0; j < closeMat[i].size(); j++) {
        bool isFar = CloseFarCheck(mAux, node, closeMat[i][j]);

        if (isFar) {
          farMat[i].push_back(closeMat[i][j]);

          std::size_t nodeSizeI = mClusterTree(node, 3) - mClusterTree(node, 2);
          std::size_t nodeSizeJ =
              mClusterTree(closeMat[i][j], 3) - mClusterTree(closeMat[i][j], 2);

          total_entry += nodeSizeI * nodeSizeJ;
        } else {
          if (mClusterTree(closeMat[i][j], 0) != 0) {
            childCloseMat.push_back(mClusterTree(closeMat[i][j], 0));
            childCloseMat.push_back(mClusterTree(closeMat[i][j], 1));
          } else {
            childCloseMat.push_back(closeMat[i][j]);
          }
        }
      }
      closeMat[mClusterTree(node, 0)] = childCloseMat;
      closeMat[mClusterTree(node, 1)] = childCloseMat;
    } else {
      std::vector<int> newCloseMat;

      for (int j = 0; j < closeMat[i].size(); j++) {
        bool isFar = CloseFarCheck(mAux, node, closeMat[i][j]);

        std::size_t nodeSizeI = mClusterTree(node, 3) - mClusterTree(node, 2);
        std::size_t nodeSizeJ =
            mClusterTree(closeMat[i][j], 3) - mClusterTree(closeMat[i][j], 2);

        total_entry += nodeSizeI * nodeSizeJ;

        if (isFar)
          farMat[i].push_back(closeMat[i][j]);
        else {
          // need to make sure this is a leaf node for close mat.
          if (mClusterTree(closeMat[i][j], 0) == 0)
            newCloseMat.push_back(closeMat[i][j]);
          else {
            std::vector<int> childCloseMat;
            std::stack<int> workChildStack;
            workChildStack.push(closeMat[i][j]);

            while (workChildStack.size() != 0) {
              auto childNode = workChildStack.top();
              workChildStack.pop();

              if (mClusterTree(childNode, 0) != 0) {
                workChildStack.push(mClusterTree(childNode, 0));
                workChildStack.push(mClusterTree(childNode, 1));
              } else {
                childCloseMat.push_back(childNode);
              }
            }

            for (int k = 0; k < childCloseMat.size(); k++)
              newCloseMat.push_back(childCloseMat[k]);
          }
        }
      }

      closeMat[i] = newCloseMat;

      mLeafNodeList.push_back(node);
    }
  }

  if (mMPIRank == 0) {
    std::cout << "total entry: " << total_entry << std::endl;

    std::cout << "num of leaf nodes: " << mLeafNodeList.size() << std::endl;
  }

  // split leaf node among mpi ranks
  std::size_t leafNodeStart, leafNodeStartI, leafNodeStartJ;
  std::size_t leafNodeEnd, leafNodeEndI, leafNodeEndJ;

  std::size_t totalCloseMatSize = 0;
  for (int i = 0; i < mLeafNodeList.size(); i++)
    totalCloseMatSize += closeMat[mLeafNodeList[i]].size();

  std::size_t estimatedLeafNodeSize =
      std::ceil((double)totalCloseMatSize / mMPISize);
  leafNodeStart = estimatedLeafNodeSize * mMPIRank;
  leafNodeEnd =
      std::min(estimatedLeafNodeSize * (mMPIRank + 1), totalCloseMatSize);

  int counter = 0;
  for (int i = 0; i < mLeafNodeList.size(); i++) {
    counter += closeMat[mLeafNodeList[i]].size();
    if (leafNodeStart <= counter) {
      leafNodeStartI = i;
      leafNodeStartJ =
          leafNodeStart - (counter - closeMat[mLeafNodeList[i]].size());
      break;
    }
  }

  counter = 0;
  for (int i = 0; i < mLeafNodeList.size(); i++) {
    counter += closeMat[mLeafNodeList[i]].size();
    if (leafNodeEnd <= counter) {
      leafNodeEndI = i;
      leafNodeEndJ =
          leafNodeEnd - (counter - closeMat[mLeafNodeList[i]].size());
      break;
    }
  }

  std::size_t leafNodeSize = leafNodeEndI - leafNodeStartI + 1;

  mLeafNodePtr = std::make_shared<DeviceIndexVector>("mLeafNode", leafNodeSize);

  DeviceIndexVector::HostMirror hostLeafNode =
      Kokkos::create_mirror_view(*mLeafNodePtr);

  auto &mLeafNode = *mLeafNodePtr;

  for (int i = 0; i < leafNodeSize; i++)
    hostLeafNode(i) = mLeafNodeList[i + leafNodeStartI];
  Kokkos::deep_copy(mLeafNode, hostLeafNode);

  mCloseMatIPtr =
      std::make_shared<DeviceIndexVector>("mCloseMatI", leafNodeSize + 1);
  auto &mCloseMatI = *mCloseMatIPtr;
  Kokkos::resize(*mCloseMatIPtr, leafNodeSize + 1);

  DeviceIndexVector::HostMirror hostCloseMatI =
      Kokkos::create_mirror_view(*mCloseMatIPtr);
  hostCloseMatI(0) = 0;
  for (int i = 0; i < leafNodeSize; i++) {
    if (i == 0)
      hostCloseMatI(i + 1) =
          closeMat[mLeafNodeList[i + leafNodeStartI]].size() - leafNodeStartJ;
    else if (i == leafNodeSize - 1)
      hostCloseMatI(i + 1) = hostCloseMatI(i) + leafNodeEndJ;
    else
      hostCloseMatI(i + 1) =
          hostCloseMatI(i) + closeMat[mLeafNodeList[i + leafNodeStartI]].size();
  }

  mCloseMatJPtr = std::make_shared<DeviceIndexVector>();
  auto &mCloseMatJ = *mCloseMatJPtr;
  Kokkos::resize(*mCloseMatJPtr, hostCloseMatI(leafNodeSize));

  DeviceIndexVector::HostMirror hostCloseMatJ =
      Kokkos::create_mirror_view(*mCloseMatJPtr);
  for (int i = 0; i < leafNodeSize; i++) {
    if (i == 0)
      for (int j = leafNodeStartJ;
           j < closeMat[mLeafNodeList[i + leafNodeStartI]].size(); j++)
        hostCloseMatJ(hostCloseMatI(i) + j - leafNodeStartJ) =
            closeMat[mLeafNodeList[i + leafNodeStartI]][j];
    else if (i == leafNodeSize - 1)
      for (int j = 0; j < leafNodeEndJ; j++)
        hostCloseMatJ(hostCloseMatI(i) + j) =
            closeMat[mLeafNodeList[i + leafNodeStartI]][j];
    else
      for (int j = 0; j < closeMat[mLeafNodeList[i + leafNodeStartI]].size();
           j++)
        hostCloseMatJ(hostCloseMatI(i) + j) =
            closeMat[mLeafNodeList[i + leafNodeStartI]][j];
  }

  if (mMPIRank == 0)
    std::cout << "Total close pair: " << hostCloseMatI(mLeafNodeList.size())
              << std::endl;

  std::size_t totalCloseEntry = 0;
  for (int i = 0; i < mLeafNodeList.size(); i++) {
    int nodeI = mLeafNodeList[i];
    int nodeSizeI = mClusterTree(nodeI, 3) - mClusterTree(nodeI, 2);
    for (int j = 0; j < closeMat[nodeI].size(); j++) {
      int nodeJ = closeMat[nodeI][j];
      int nodeSizeJ = mClusterTree(nodeJ, 3) - mClusterTree(nodeJ, 2);

      totalCloseEntry += nodeSizeI * nodeSizeJ;
    }
  }

  if (mMPIRank == 0)
    std::cout << "Total close entry: " << totalCloseEntry << std::endl;

  Kokkos::deep_copy(mCloseMatI, hostCloseMatI);
  Kokkos::deep_copy(mCloseMatJ, hostCloseMatJ);

  std::size_t totalFarSize = 0;
  std::size_t totalFarNode = 0;
  for (int i = 0; i < farMat.size(); i++) {
    if (farMat[i].size() != 0) {
      totalFarSize += farMat[i].size();
      totalFarNode++;
    }
  }
  if (mMPIRank == 0) {
    std::cout << "Total far node: " << totalFarNode << std::endl;
    std::cout << "Total far pair: " << totalFarSize << std::endl;
  }

  // split far pair among mpi ranks
  std::size_t farMatSize = totalFarSize / (std::size_t)mMPISize;
  farMatSize += ((totalFarSize % (std::size_t)mMPISize) > mMPIRank) ? 1 : 0;

  mFarMatIPtr = std::make_shared<DeviceIndexVector>("mFarMatI", farMatSize);
  auto &mFarMatI = *mFarMatIPtr;

  mFarMatJPtr = std::make_shared<DeviceIndexVector>("mFarMatJ", farMatSize);
  auto &mFarMatJ = *mFarMatJPtr;

  DeviceIndexVector::HostMirror farMatIMirror =
      Kokkos::create_mirror_view(mFarMatI);
  DeviceIndexVector::HostMirror farMatJMirror =
      Kokkos::create_mirror_view(mFarMatJ);

  counter = 0;
  int matCounter = 0;
  for (int i = 0; i < farMat.size(); i++) {
    for (int j = 0; j < farMat[i].size(); j++) {
      if (counter % mMPISize == mMPIRank) {
        farMatIMirror(matCounter) = i;
        farMatJMirror(matCounter) = farMat[i][j];
        matCounter++;
      }
      counter++;
    }
  }

  std::size_t farDotQueryNum = 0;
  for (int i = 0; i < farMatIMirror.extent(0); i++) {
    std::size_t nodeISize =
        mClusterTree(farMatIMirror(i), 3) - mClusterTree(farMatIMirror(i), 2);
    std::size_t nodeJSize =
        mClusterTree(farMatJMirror(i), 3) - mClusterTree(farMatJMirror(i), 2);
    farDotQueryNum += nodeISize * nodeJSize;
  }
  MPI_Allreduce(MPI_IN_PLACE, &farDotQueryNum, 1, MPI_UNSIGNED_LONG, MPI_SUM,
                MPI_COMM_WORLD);
  if (mMPIRank == 0)
    std::cout << "Total far entry: " << farDotQueryNum << std::endl;

  Kokkos::deep_copy(mFarMatI, farMatIMirror);
  Kokkos::deep_copy(mFarMatJ, farMatJMirror);

  std::chrono::high_resolution_clock::time_point t2 =
      std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
  if (mMPIRank == 0) {
    std::cout << "Time for building close and far matrix: "
              << (double)duration / 1e6 << "s" << std::endl;
  }
}