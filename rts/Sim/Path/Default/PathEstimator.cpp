/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/Platform/Win/win32.h"

#include "minizip/zip.h"

#include "PathEstimator.h"
#include "PathFinder.h"
#include "PathFinderDef.h"
// #include "PathFlowMap.hpp"
#include "PathLog.h"
#include "PathMemPool.h"
#include "Game/GlobalUnsynced.h"
#include "Game/LoadScreen.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "Net/Protocol/NetProtocol.h"
#include "System/Threading/ThreadPool.h" // for_mt
#include "System/TimeProfiler.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/Archives/IArchive.h"
#include "System/FileSystem/ArchiveLoader.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileSystem.h"
#include "System/FileSystem/FileQueryFlags.h"
#include "System/Platform/Threading.h"
#include "System/SafeUtil.h"
#include "System/StringUtil.h"
#include "System/Sync/HsiehHash.h"
#include "System/Sync/SHA512.hpp"

#define ENABLE_NETLOG_CHECKSUM 1


CONFIG(int, MaxPathCostsMemoryFootPrint).defaultValue(512).minimumValue(64).description("Maximum memusage (in MByte) of multithreaded pathcache generator at loading time.");

PCMemPool pcMemPool;
PEMemPool peMemPool;


static const std::string GetPathCacheDir() {
	return (FileSystem::GetCacheDir() + "/paths/");
}

static size_t GetNumThreads() {
	const size_t numThreads = std::max(0, configHandler->GetInt("PathingThreadCount"));
	const size_t numCores = Threading::GetLogicalCpuCores();
	return ((numThreads == 0)? numCores: numThreads);
}



CPathEstimator::CPathEstimator(IPathFinder* pf, unsigned int BLOCK_SIZE, const std::string& cacheFileName, const std::string& mapFileName)
	: IPathFinder(BLOCK_SIZE)
	, BLOCKS_TO_UPDATE(SQUARES_TO_UPDATE / (BLOCK_SIZE * BLOCK_SIZE) + 1)
	, nextOffsetMessageIdx(0)
	, nextCostMessageIdx(0)
	, pathChecksum(0)
	, fileHashCode(CalcHash(__func__))
	, offsetBlockNum(nbrOfBlocks.x * nbrOfBlocks.y)
	, costBlockNum(nbrOfBlocks.x * nbrOfBlocks.y)
	, pathFinder(pf)
	, nextPathEstimator(nullptr)
	, blockUpdatePenalty(0)
{
	vertexCosts.resize(moveDefHandler->GetNumMoveDefs() * blockStates.GetSize() * PATH_DIRECTION_VERTICES, PATHCOST_INFINITY);
	maxSpeedMods.resize(moveDefHandler->GetNumMoveDefs(), 0.001f);

	CPathEstimator*  childPE = this;
	CPathEstimator* parentPE = dynamic_cast<CPathEstimator*>(pf);

	if (parentPE != nullptr)
		parentPE->nextPathEstimator = childPE;

	// precalc for FindOffset()
	{
		offsetBlocksSortedByCost.reserve(BLOCK_SIZE * BLOCK_SIZE);
		for (unsigned int z = 0; z < BLOCK_SIZE; ++z) {
			for (unsigned int x = 0; x < BLOCK_SIZE; ++x) {
				const float dx = x - (float)(BLOCK_SIZE - 1) * 0.5f;
				const float dz = z - (float)(BLOCK_SIZE - 1) * 0.5f;
				const float cost = (dx * dx + dz * dz);

				offsetBlocksSortedByCost.emplace_back(cost, x, z);
			}
		}
		std::stable_sort(offsetBlocksSortedByCost.begin(), offsetBlocksSortedByCost.end(), [](const SOffsetBlock& a, const SOffsetBlock& b) {
			return (a.cost < b.cost);
		});
	}

	if (BLOCK_SIZE == LOWRES_PE_BLOCKSIZE) {
		assert(parentPE != nullptr);

		// calculate map-wide maximum positional speedmod for each MoveDef
		for_mt(0, moveDefHandler->GetNumMoveDefs(), [&](unsigned int i) {
			const MoveDef* md = moveDefHandler->GetMoveDefByPathType(i);

			if (md->udRefCount == 0)
				return;

			for (int y = 0; y < mapDims.mapy; y++) {
				for (int x = 0; x < mapDims.mapx; x++) {
					childPE->maxSpeedMods[i] = std::max(childPE->maxSpeedMods[i], CMoveMath::GetPosSpeedMod(*md, x, y));
				}
			}
		});

		// calculate reciprocals, avoids divisions in TestBlock
		for (unsigned int i = 0; i < maxSpeedMods.size(); i++) {
			 childPE->maxSpeedMods[i] = 1.0f / childPE->maxSpeedMods[i];
			parentPE->maxSpeedMods[i] = childPE->maxSpeedMods[i];
		}
	}

	// load precalculated data if it exists
	InitEstimator(cacheFileName, mapFileName);
}


CPathEstimator::~CPathEstimator()
{
	pcMemPool.free(pathCache[0]);
	pcMemPool.free(pathCache[1]);
}


const int2* CPathEstimator::GetDirectionVectorsTable() {
	return (&PE_DIRECTION_VECTORS[0]);
}


void CPathEstimator::InitEstimator(const std::string& cacheFileName, const std::string& mapName)
{
	const unsigned int numThreads = GetNumThreads();

	if (threads.size() != numThreads) {
		threads.resize(numThreads);
		pathFinders.resize(numThreads);
	}

	// always use PF for initialization, later PE maybe used
	pathFinders[0] = pfMemPool.alloc<CPathFinder>();

	// Not much point in multithreading these...
	InitBlocks();

	if (!ReadFile(cacheFileName, mapName)) {
		// start extra threads if applicable, but always keep the total
		// memory-footprint made by CPathFinder instances within bounds
		const unsigned int minMemFootPrint = sizeof(CPathFinder) + pathFinder->GetMemFootPrint();
		const unsigned int maxMemFootPrint = configHandler->GetInt("MaxPathCostsMemoryFootPrint") * 1024 * 1024;
		const unsigned int numExtraThreads = Clamp(int(maxMemFootPrint / minMemFootPrint) - 1, 0, int(numThreads) - 1);
		const unsigned int reqMemFootPrint = minMemFootPrint * (numExtraThreads + 1);

		char calcMsg[512];
		const char* fmtStrs[4] = {
			"[%s] creating PE%u cache with %u PF threads (%u MB)",
			"[%s] creating PE%u cache with %u PF thread (%u MB)",
			"[%s] writing PE%u %s-cache to file",
			"[%s] written PE%u %s-cache to file",
		};

		{
			sprintf(calcMsg, fmtStrs[numExtraThreads == 0], __func__, BLOCK_SIZE, numExtraThreads + 1, reqMemFootPrint / (1024 * 1024));
			loadscreen->SetLoadMessage(calcMsg);
		}

		// note: only really needed if numExtraThreads > 0
		pathBarrier = new spring::barrier(numExtraThreads + 1);

		for (unsigned int i = 1; i <= numExtraThreads; i++) {
			pathFinders[i] = pfMemPool.alloc<CPathFinder>();
			threads[i] = new spring::thread(std::bind(&CPathEstimator::CalcOffsetsAndPathCosts, this, i));
		}

		// Use the current thread as thread zero
		CalcOffsetsAndPathCosts(0);

		for (unsigned int i = 1; i <= numExtraThreads; i++) {
			threads[i]->join();
			delete threads[i];
			pfMemPool.free(pathFinders[i]);
		}

		delete pathBarrier;


		sprintf(calcMsg, fmtStrs[2], __func__, BLOCK_SIZE, cacheFileName.c_str());
		loadscreen->SetLoadMessage(calcMsg, true);

		WriteFile(cacheFileName, mapName);

		sprintf(calcMsg, fmtStrs[3], __func__, BLOCK_SIZE, cacheFileName.c_str());
		loadscreen->SetLoadMessage(calcMsg, true);
	}

	// Calculate PreCached PathData Checksum
	pathChecksum = CalcChecksum();

	// switch to runtime wanted IPathFinder (maybe PF or PE)
	pfMemPool.free(pathFinders[0]);
	pathFinders[0] = pathFinder;

	pathCache[0] = pcMemPool.alloc<CPathCache>(nbrOfBlocks.x, nbrOfBlocks.y);
	pathCache[1] = pcMemPool.alloc<CPathCache>(nbrOfBlocks.x, nbrOfBlocks.y);
}


void CPathEstimator::InitBlocks()
{
	blockStates.peNodeOffsets.resize(moveDefHandler->GetNumMoveDefs());
	for (unsigned int idx = 0; idx < moveDefHandler->GetNumMoveDefs(); idx++) {
		blockStates.peNodeOffsets[idx].resize(nbrOfBlocks.x * nbrOfBlocks.y);
	}
}


__FORCE_ALIGN_STACK__
void CPathEstimator::CalcOffsetsAndPathCosts(unsigned int threadNum)
{
	// reset FPU state for synced computations
	streflop::streflop_init<streflop::Simple>();

	if (threadNum > 0) {
		Threading::SetAffinity(~0);
		Threading::SetThreadName(IntToString(threadNum, "pathhelper%i"));
	}

	// NOTE: EstimatePathCosts() [B] is temporally dependent on CalculateBlockOffsets() [A],
	// A must be completely finished before B_i can be safely called. This means we cannot
	// let thread i execute (A_i, B_i), but instead have to split the work such that every
	// thread finishes its part of A before any starts B_i.
	const unsigned int maxBlockIdx = blockStates.GetSize() - 1;
	int i;

	while ((i = --offsetBlockNum) >= 0)
		CalculateBlockOffsets(maxBlockIdx - i, threadNum);

	pathBarrier->wait();

	while ((i = --costBlockNum) >= 0)
		EstimatePathCosts(maxBlockIdx - i, threadNum);
}


void CPathEstimator::CalculateBlockOffsets(unsigned int blockIdx, unsigned int threadNum)
{
	const int2 blockPos = BlockIdxToPos(blockIdx);

	if (threadNum == 0 && blockIdx >= nextOffsetMessageIdx) {
		nextOffsetMessageIdx = blockIdx + blockStates.GetSize() / 16;
		clientNet->Send(CBaseNetProtocol::Get().SendCPUUsage(BLOCK_SIZE | (blockIdx << 8)));
	}

	for (unsigned int i = 0; i < moveDefHandler->GetNumMoveDefs(); i++) {
		const MoveDef* md = moveDefHandler->GetMoveDefByPathType(i);

		if (md->udRefCount > 0) {
			blockStates.peNodeOffsets[md->pathType][blockIdx] = FindOffset(*md, blockPos.x, blockPos.y);
		}
	}
}


void CPathEstimator::EstimatePathCosts(unsigned int blockIdx, unsigned int threadNum)
{
	const int2 blockPos = BlockIdxToPos(blockIdx);

	if (threadNum == 0 && blockIdx >= nextCostMessageIdx) {
		nextCostMessageIdx = blockIdx + blockStates.GetSize() / 16;

		char calcMsg[128];
		sprintf(calcMsg, "[%s] precached %d of %d blocks", __func__, blockIdx, blockStates.GetSize());

		clientNet->Send(CBaseNetProtocol::Get().SendCPUUsage(0x1 | BLOCK_SIZE | (blockIdx << 8)));
		loadscreen->SetLoadMessage(calcMsg, (blockIdx != 0));
	}

	for (unsigned int i = 0; i < moveDefHandler->GetNumMoveDefs(); i++) {
		const MoveDef* md = moveDefHandler->GetMoveDefByPathType(i);

		if (md->udRefCount > 0) {
			CalculateVertices(*md, blockPos, threadNum);
		}
	}
}


/**
 * Move around the blockPos a bit, so we `surround` unpassable blocks.
 */
int2 CPathEstimator::FindOffset(const MoveDef& moveDef, unsigned int blockX, unsigned int blockZ) const
{
	// lower corner position of block
	const unsigned int lowerX = blockX * BLOCK_SIZE;
	const unsigned int lowerZ = blockZ * BLOCK_SIZE;
	const unsigned int blockArea = (BLOCK_SIZE * BLOCK_SIZE) / SQUARE_SIZE;

	int2 bestPos(lowerX + (BLOCK_SIZE >> 1), lowerZ + (BLOCK_SIZE >> 1));
	float bestCost = std::numeric_limits<float>::max();

	// search for an accessible position within this block
	/*for (unsigned int z = 0; z < BLOCK_SIZE; ++z) {
		for (unsigned int x = 0; x < BLOCK_SIZE; ++x) {
			float speedMod = CMoveMath::GetPosSpeedMod(moveDef, lowerX + x, lowerZ + z);
			bool curblock = (speedMod == 0.0f) || CMoveMath::IsBlockedStructure(moveDef, lowerX + x, lowerZ + z, nullptr);

			if (!curblock) {
				const float dx = x - (float)(BLOCK_SIZE - 1) * 0.5f;
				const float dz = z - (float)(BLOCK_SIZE - 1) * 0.5f;
				const float cost = (dx * dx + dz * dz) + (blockArea / (0.001f + speedMod));

				if (cost < bestCost) {
					bestCost = cost;
					bestPos.x = lowerX + x;
					bestPos.y = lowerZ + z;
				}
			}
		}
	}*/

	// Equal code
	// just that we have sorted the squares by their baseCost, so
	// can early exit, when it increases above our current best one.
	// Performance: tests showed that only ~60% need to be tested
	for (const SOffsetBlock& ob: offsetBlocksSortedByCost) {
		if (ob.cost >= bestCost)
			break;

		const int2 blockPos(lowerX + ob.offset.x, lowerZ + ob.offset.y);
		const float speedMod = CMoveMath::GetPosSpeedMod(moveDef, blockPos.x, blockPos.y);

		//assert((blockArea / (0.001f + speedMod) >= 0.0f);
		const float cost = ob.cost + (blockArea / (0.001f + speedMod));

		if (cost >= bestCost)
			continue;

		if (!CMoveMath::IsBlockedStructure(moveDef, blockPos.x, blockPos.y, nullptr)) {
			bestCost = cost;
			bestPos  = blockPos;
		}
	}

	// return the offset found
	return bestPos;
}


/**
 * Calculate all vertices connected from the given block
 */
void CPathEstimator::CalculateVertices(const MoveDef& moveDef, int2 block, unsigned int thread)
{
	// see code comment of GetBlockVertexOffset() for more info why those directions are choosen
	CalculateVertex(moveDef, block, PATHDIR_LEFT,     thread);
	CalculateVertex(moveDef, block, PATHDIR_LEFT_UP,  thread);
	CalculateVertex(moveDef, block, PATHDIR_UP,       thread);
	CalculateVertex(moveDef, block, PATHDIR_RIGHT_UP, thread);
}


/**
 * Calculate requested vertex
 */
void CPathEstimator::CalculateVertex(
	const MoveDef& moveDef,
	int2 parentBlock,
	unsigned int direction,
	unsigned int threadNum)
{
	const int2 childBlock = parentBlock + PE_DIRECTION_VECTORS[direction];
	const unsigned int parentBlockNbr = BlockPosToIdx(parentBlock);
	const unsigned int childBlockNbr  = BlockPosToIdx(childBlock);
	const unsigned int vertexNbr =
		moveDef.pathType * blockStates.GetSize() * PATH_DIRECTION_VERTICES +
		parentBlockNbr * PATH_DIRECTION_VERTICES +
		direction;

	// outside map?
	if ((unsigned)childBlock.x >= nbrOfBlocks.x || (unsigned)childBlock.y >= nbrOfBlocks.y) {
		vertexCosts[vertexNbr] = PATHCOST_INFINITY;
		return;
	}


	// start position within parent block
	const int2 parentSquare = blockStates.peNodeOffsets[moveDef.pathType][parentBlockNbr];

	// goal position within child block
	const int2 childSquare = blockStates.peNodeOffsets[moveDef.pathType][childBlockNbr];
	const float3 startPos  = SquareToFloat3(parentSquare.x, parentSquare.y);
	const float3 goalPos   = SquareToFloat3(childSquare.x, childSquare.y);

	// keep search exactly contained within the two blocks
	CRectangularSearchConstraint pfDef(startPos, goalPos, 0.0f, BLOCK_SIZE);

	// we never want to allow searches from any blocked starting positions
	// (otherwise PE and PF can disagree), but are more lenient for normal
	// searches so players can "unstuck" units
	// note: PE itself should ensure this never happens to begin with?
	//
	// blocked goal positions are always early-outs (no searching needed)
	const bool strtBlocked = ((CMoveMath::IsBlocked(moveDef, startPos, nullptr) & CMoveMath::BLOCK_STRUCTURE) != 0);
	const bool goalBlocked = pfDef.IsGoalBlocked(moveDef, CMoveMath::BLOCK_STRUCTURE, nullptr);
	if (strtBlocked || goalBlocked) {
		vertexCosts[vertexNbr] = PATHCOST_INFINITY;
		return;
	}

	// find path from parent to child block
	//
	// since CPathFinder::GetPath() is not thread-safe, use
	// this thread's "private" CPathFinder instance (rather
	// than locking pathFinder->GetPath()) if we are in one
	pfDef.testMobile     = false;
	pfDef.needPath       = false;
	pfDef.exactPath      = true;
	pfDef.dirIndependent = true;

	IPath::Path path;
	IPath::SearchResult result = pathFinders[threadNum]->GetPath(moveDef, pfDef, nullptr, startPos, path, MAX_SEARCHED_NODES_PF >> 2);

	// store the result
	if (result == IPath::Ok) {
		vertexCosts[vertexNbr] = path.pathCost;
	} else {
		vertexCosts[vertexNbr] = PATHCOST_INFINITY;
	}
}


/**
 * Mark affected blocks as obsolete
 */
void CPathEstimator::MapChanged(unsigned int x1, unsigned int z1, unsigned int x2, unsigned z2)
{
	// find the upper and lower corner of the rectangular area
	const auto mmx = std::minmax(x1, x2);
	const auto mmz = std::minmax(z1, z2);
	const int lowerX = Clamp(int(mmx.first  / BLOCK_SIZE) - 1, 0, int(nbrOfBlocks.x - 1));
	const int upperX = Clamp(int(mmx.second / BLOCK_SIZE) + 1, 0, int(nbrOfBlocks.x - 1));
	const int lowerZ = Clamp(int(mmz.first  / BLOCK_SIZE) - 1, 0, int(nbrOfBlocks.y - 1));
	const int upperZ = Clamp(int(mmz.second / BLOCK_SIZE) + 1, 0, int(nbrOfBlocks.y - 1));

	// mark the blocks inside the rectangle, enqueue them
	// from upper to lower because of the placement of the
	// bi-directional vertices
	for (int z = upperZ; z >= lowerZ; z--) {
		for (int x = upperX; x >= lowerX; x--) {
			const int idx = BlockPosToIdx(int2(x,z));
			if ((blockStates.nodeMask[idx] & PATHOPT_OBSOLETE) != 0)
				continue;

			updatedBlocks.emplace_back(x, z);
			blockStates.nodeMask[idx] |= PATHOPT_OBSOLETE;
		}
	}
}


/**
 * Update some obsolete blocks using the FIFO-principle
 */
void CPathEstimator::Update()
{
	pathCache[0]->Update();
	pathCache[1]->Update();

	const unsigned int numMoveDefs = moveDefHandler->GetNumMoveDefs();

	if (numMoveDefs == 0)
		return;

	// determine how many blocks we should update
	int blocksToUpdate = 0;
	int consumeBlocks = 0;
	{
		const int progressiveUpdates = updatedBlocks.size() * numMoveDefs * modInfo.pfUpdateRate;
		const int MIN_BLOCKS_TO_UPDATE = std::max<int>(BLOCKS_TO_UPDATE >> 1, 4U);
		const int MAX_BLOCKS_TO_UPDATE = std::max<int>(BLOCKS_TO_UPDATE << 1, MIN_BLOCKS_TO_UPDATE);
		blocksToUpdate = Clamp(progressiveUpdates, MIN_BLOCKS_TO_UPDATE, MAX_BLOCKS_TO_UPDATE);

		blockUpdatePenalty = std::max(0, blockUpdatePenalty - blocksToUpdate);

		if (blockUpdatePenalty > 0)
			blocksToUpdate = std::max(0, blocksToUpdate - blockUpdatePenalty);

		// we have to update blocks for all movedefs (cause PATHOPT_OBSOLETE is per block and not movedef)
		consumeBlocks = int(progressiveUpdates != 0) * int(ceil(float(blocksToUpdate) / numMoveDefs)) * numMoveDefs;

		blockUpdatePenalty += consumeBlocks;
	}

	if (blocksToUpdate == 0)
		return;

	if (updatedBlocks.empty())
		return;

	struct SingleBlock {
		int2 blockPos;
		const MoveDef* moveDef;
		SingleBlock(const int2& pos, const MoveDef* md) : blockPos(pos), moveDef(md) {}
	};
	std::vector<SingleBlock> consumedBlocks;
	consumedBlocks.reserve(consumeBlocks);

	// get blocks to update
	while (!updatedBlocks.empty()) {
		int2& pos = updatedBlocks.front();
		const int idx = BlockPosToIdx(pos);

		if ((blockStates.nodeMask[idx] & PATHOPT_OBSOLETE) == 0) {
			updatedBlocks.pop_front();
			continue;
		}

		if (consumedBlocks.size() >= blocksToUpdate)
			break;

		// issue repathing for all active movedefs
		for (unsigned int i = 0; i < numMoveDefs; i++) {
			const MoveDef* md = moveDefHandler->GetMoveDefByPathType(i);
			if (md->udRefCount > 0)
				consumedBlocks.emplace_back(pos, md);
		}

		// inform dependent pathEstimator that we change vertex cost of those blocks
		if (nextPathEstimator)
			nextPathEstimator->MapChanged(pos.x * BLOCK_SIZE, pos.y * BLOCK_SIZE, pos.x * BLOCK_SIZE, pos.y * BLOCK_SIZE);

		updatedBlocks.pop_front(); // must happen _after_ last usage of the `pos` reference!
		blockStates.nodeMask[idx] &= ~PATHOPT_OBSOLETE;
	}

	// FindOffset (threadsafe)
	{
		SCOPED_TIMER("Sim::Path::Estimator::FindOffset");
		for_mt(0, consumedBlocks.size(), [&](const int n) {
			// copy the next block in line
			const SingleBlock sb = consumedBlocks[n];
			const int blockN = BlockPosToIdx(sb.blockPos);
			const MoveDef* currBlockMD = sb.moveDef;
			blockStates.peNodeOffsets[currBlockMD->pathType][blockN] = FindOffset(*currBlockMD, sb.blockPos.x, sb.blockPos.y);
		});
	}

	// CalculateVertices (not threadsafe)
	{
		SCOPED_TIMER("Sim::Path::Estimator::CalculateVertices");
		for (unsigned int n = 0; n < consumedBlocks.size(); ++n) {
			// copy the next block in line
			const SingleBlock sb = consumedBlocks[n];
			CalculateVertices(*sb.moveDef, sb.blockPos);
		}
	}
}


const CPathCache::CacheItem& CPathEstimator::GetCache(const int2 strtBlock, const int2 goalBlock, float goalRadius, int pathType, const bool synced) const
{
	return pathCache[synced]->GetCachedPath(strtBlock, goalBlock, goalRadius, pathType);
}

void CPathEstimator::AddCache(const IPath::Path* path, const IPath::SearchResult result, const int2 strtBlock, const int2 goalBlock, float goalRadius, int pathType, const bool synced)
{
	pathCache[synced]->AddPath(path, result, strtBlock, goalBlock, goalRadius, pathType);
}


/**
 * Performs the actual search.
 */
IPath::SearchResult CPathEstimator::DoSearch(const MoveDef& moveDef, const CPathFinderDef& peDef, const CSolidObject* owner)
{
	bool foundGoal = false;

	// get the goal square offset
	const int2 goalSqrOffset = peDef.GoalSquareOffset(BLOCK_SIZE);
	const float maxSpeedMod = maxSpeedMods[moveDef.pathType];

	while (!openBlocks.empty() && (openBlockBuffer.GetSize() < maxBlocksToBeSearched)) {
		// get the open block with lowest cost
		PathNode* ob = const_cast<PathNode*>(openBlocks.top());
		openBlocks.pop();

		// check if the block has been marked as unaccessible during its time in the queue
		if (blockStates.nodeMask[ob->nodeNum] & (PATHOPT_BLOCKED | PATHOPT_CLOSED))
			continue;

		// no, check if the goal is already reached
		const int2 bSquare = blockStates.peNodeOffsets[moveDef.pathType][ob->nodeNum];
		const int2 gSquare = ob->nodePos * BLOCK_SIZE + goalSqrOffset;

		if (peDef.IsGoal(bSquare.x, bSquare.y) || peDef.IsGoal(gSquare.x, gSquare.y)) {
			mGoalBlockIdx = ob->nodeNum;
			mGoalHeuristic = 0.0f;
			foundGoal = true;
			break;
		}

		// no, test the 8 surrounding blocks
		// NOTE: each of these calls increments openBlockBuffer.idx by 1, so
		// maxBlocksToBeSearched is always less than <MAX_SEARCHED_NODES_PE - 8>
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_LEFT,       PATHOPT_OPEN, maxSpeedMod);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_LEFT_UP,    PATHOPT_OPEN, maxSpeedMod);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_UP,         PATHOPT_OPEN, maxSpeedMod);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_RIGHT_UP,   PATHOPT_OPEN, maxSpeedMod);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_RIGHT,      PATHOPT_OPEN, maxSpeedMod);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_RIGHT_DOWN, PATHOPT_OPEN, maxSpeedMod);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_DOWN,       PATHOPT_OPEN, maxSpeedMod);
		TestBlock(moveDef, peDef, ob, owner, PATHDIR_LEFT_DOWN,  PATHOPT_OPEN, maxSpeedMod);

		// mark this block as closed
		blockStates.nodeMask[ob->nodeNum] |= PATHOPT_CLOSED;
	}

	// we found our goal
	if (foundGoal)
		return IPath::Ok;

	// we could not reach the goal
	if (openBlockBuffer.GetSize() >= maxBlocksToBeSearched)
		return IPath::GoalOutOfRange;

	// search could not reach the goal due to the unit being locked in
	if (openBlocks.empty())
		return IPath::GoalOutOfRange;

	// should never happen
	LOG_L(L_ERROR, "%s - Unhandled end of search!", __func__);
	return IPath::Error;
}


/**
 * Test the accessability of a block and its value,
 * possibly also add it to the open-blocks pqueue.
 */
bool CPathEstimator::TestBlock(
	const MoveDef& moveDef,
	const CPathFinderDef& peDef,
	const PathNode* parentOpenBlock,
	const CSolidObject* owner,
	const unsigned int pathDir,
	const unsigned int /*blockStatus*/,
	float maxSpeedMod
) {
	testedBlocks++;

	// initial calculations of the new block
	const int2 testBlockPos = parentOpenBlock->nodePos + PE_DIRECTION_VECTORS[pathDir];
	const int2 goalBlockPos = {int(peDef.goalSquareX / BLOCK_SIZE), int(peDef.goalSquareZ / BLOCK_SIZE)};

	const unsigned int testBlockIdx = BlockPosToIdx(testBlockPos);

	// bounds-check
	if ((unsigned)testBlockPos.x >= nbrOfBlocks.x) return false;
	if ((unsigned)testBlockPos.y >= nbrOfBlocks.y) return false;

	// check if the block is unavailable
	if (blockStates.nodeMask[testBlockIdx] & (PATHOPT_BLOCKED | PATHOPT_CLOSED))
		return false;

	// read precached vertex costs
	const unsigned int pathTypeBaseIdx = moveDef.pathType * blockStates.GetSize() * PATH_DIRECTION_VERTICES;
	const unsigned int blockNumBaseIdx = parentOpenBlock->nodeNum * PATH_DIRECTION_VERTICES;
	const unsigned int vertexIdx = pathTypeBaseIdx + blockNumBaseIdx + GetBlockVertexOffset(pathDir, nbrOfBlocks.x);
	assert(vertexIdx < vertexCosts.size());


	if (vertexCosts[vertexIdx] >= PATHCOST_INFINITY) {
		// warning: we cannot naively set PATHOPT_BLOCKED here;
		// vertexCosts[] depends on the direction and nodeMask
		// does not
		// we would have to save the direction via PATHOPT_LEFT
		// etc. in the nodeMask but that is complicated and not
		// worth it: would just save the vertexCosts[] lookup
		//
		// blockStates.nodeMask[testBlockIdx] |= (PathDir2PathOpt(pathDir) | PATHOPT_BLOCKED);
		// dirtyBlocks.push_back(testBlockIdx);
		return false;
	}

	// check if the block is out of constraints
	const int2 square = blockStates.peNodeOffsets[moveDef.pathType][testBlockIdx];
	if (!peDef.WithinConstraints(square.x, square.y)) {
		blockStates.nodeMask[testBlockIdx] |= PATHOPT_BLOCKED;
		dirtyBlocks.push_back(testBlockIdx);
		return false;
	}

	// constraintDisabled is a hackish way to make sure we don't check this in CalculateVertices
	if (testBlockPos == goalBlockPos && peDef.needPath) {
		IPath::Path path;

		// if we have expanded the goal-block, check if a valid
		// max-resolution path exists (from where we entered it
		// to the actual goal position) since there might still
		// be impassable terrain in between
		//
		// const float3 gWorldPos = {            testBlockPos.x * BLOCK_PIXEL_SIZE * 1.0f, 0.0f,             testBlockPos.y * BLOCK_PIXEL_SIZE * 1.0f};
		// const float3 sWorldPos = {parentOpenBlock->nodePos.x * BLOCK_PIXEL_SIZE * 1.0f, 0.0f, parentOpenBlock->nodePos.y * BLOCK_PIXEL_SIZE * 1.0f};
		const int idx = BlockPosToIdx(testBlockPos);
		const int2 testSquare = blockStates.peNodeOffsets[moveDef.pathType][idx];

		const float3 sWorldPos = SquareToFloat3(testSquare.x, testSquare.y);
		const float3 gWorldPos = peDef.goal;

		if (sWorldPos.SqDistance2D(gWorldPos) > peDef.sqGoalRadius) {
			CRectangularSearchConstraint pfDef = CRectangularSearchConstraint(sWorldPos, gWorldPos, peDef.sqGoalRadius, BLOCK_SIZE); // sets goalSquare{X,Z}
			pfDef.testMobile     = false;
			pfDef.needPath       = false;
			pfDef.exactPath      = true;
			pfDef.dirIndependent = true;
			const IPath::SearchResult searchRes = pathFinder->GetPath(moveDef, pfDef, owner, sWorldPos, path, MAX_SEARCHED_NODES_PF >> 3);

			if (searchRes != IPath::Ok) {
				// we cannot set PATHOPT_BLOCKED here either, result
				// depends on direction of entry from the parent node
				//
				// blockStates.nodeMask[testBlockIdx] |= PATHOPT_BLOCKED;
				// dirtyBlocks.push_back(testBlockIdx);
				return false;
			}
		}
	}


	// evaluate this node (NOTE the max-resolution indexing for {flow,extra}Cost)
	//
	// nodeCost incorporates speed-modifiers, but the heuristic estimate does not
	// this causes an overestimation (and hence sub-optimal paths) if the average
	// modifier is greater than 1, which can only be fixed by choosing a constant
	// multiplier smaller than 1
	// however, since that would increase the number of explored nodes on "normal"
	// maps where the average is ~1, it is better to divide hCost by the (initial)
	// maximum modifier value
	//
	// const float flowCost  = (peDef.testMobile) ? (PathFlowMap::GetInstance())->GetFlowCost(square.x, square.y, moveDef, PathDir2PathOpt(pathDir)) : 0.0f;
	const float   extraCost = blockStates.GetNodeExtraCost(square.x, square.y, peDef.synced);
	const float    nodeCost = vertexCosts[vertexIdx] + extraCost;

	const float gCost = parentOpenBlock->gCost + nodeCost;
	const float hCost = peDef.Heuristic(square.x, square.y, BLOCK_SIZE) * maxSpeedMod;
	const float fCost = gCost + hCost;

	// already in the open set?
	if (blockStates.nodeMask[testBlockIdx] & PATHOPT_OPEN) {
		// check if new found path is better or worse than the old one
		if (blockStates.fCost[testBlockIdx] <= fCost)
			return true;

		// no, clear old path data
		blockStates.nodeMask[testBlockIdx] &= ~PATHOPT_CARDINALS;
	}

	// look for improvements
	if (hCost < mGoalHeuristic) {
		mGoalBlockIdx = testBlockIdx;
		mGoalHeuristic = hCost;
	}

	// store this block as open
	openBlockBuffer.SetSize(openBlockBuffer.GetSize() + 1);
	assert(openBlockBuffer.GetSize() < MAX_SEARCHED_NODES_PE);

	PathNode* ob = openBlockBuffer.GetNode(openBlockBuffer.GetSize());
		ob->fCost   = fCost;
		ob->gCost   = gCost;
		ob->nodePos = testBlockPos;
		ob->nodeNum = testBlockIdx;
	openBlocks.push(ob);

	blockStates.SetMaxCost(NODE_COST_F, std::max(blockStates.GetMaxCost(NODE_COST_F), fCost));
	blockStates.SetMaxCost(NODE_COST_G, std::max(blockStates.GetMaxCost(NODE_COST_G), gCost));

	// mark this block as open
	blockStates.fCost[testBlockIdx] = fCost;
	blockStates.gCost[testBlockIdx] = gCost;
	blockStates.nodeMask[testBlockIdx] |= (PathDir2PathOpt(pathDir) | PATHOPT_OPEN);

	dirtyBlocks.push_back(testBlockIdx);
	return true;
}


/**
 * Recreate the path taken to the goal
 */
IPath::SearchResult CPathEstimator::FinishSearch(const MoveDef& moveDef, const CPathFinderDef& pfDef, IPath::Path& foundPath) const
{
	// set some additional information
	foundPath.pathCost = blockStates.fCost[mGoalBlockIdx] - mGoalHeuristic;

	if (pfDef.needPath) {
		unsigned int blockIdx = mGoalBlockIdx;
		unsigned int numNodes = 0;

		{
			#if 1
			while (blockIdx != mStartBlockIdx) {
				const unsigned int pathOpt = blockStates.nodeMask[blockIdx] & PATHOPT_CARDINALS;
				const unsigned int pathDir = PathOpt2PathDir(pathOpt);

				blockIdx  = BlockPosToIdx(BlockIdxToPos(blockIdx) - PE_DIRECTION_VECTORS[pathDir]);
				numNodes += 1;
			}

			// PE's do not need the squares
			// foundPath.squares.reserve(numNodes);
			foundPath.path.reserve(numNodes);

			// reset
			blockIdx = mGoalBlockIdx;

			#else

			// more wasteful, but slightly faster for very long paths
			// foundPath.squares.reserve(1024 / BLOCK_SIZE);
			foundPath.path.reserve(1024 / BLOCK_SIZE);
			#endif
		}

		while (true) {
			// use offset defined by the block
			const int2 square = blockStates.peNodeOffsets[moveDef.pathType][blockIdx];

			// foundPath.squares.push_back(square);
			foundPath.path.emplace_back(square.x * SQUARE_SIZE, CMoveMath::yLevel(moveDef, square.x, square.y), square.y * SQUARE_SIZE);

			if (blockIdx == mStartBlockIdx)
				break;

			// next step backwards
			const unsigned int pathOpt = blockStates.nodeMask[blockIdx] & PATHOPT_CARDINALS;
			const unsigned int pathDir = PathOpt2PathDir(pathOpt);

			blockIdx = BlockPosToIdx(BlockIdxToPos(blockIdx) - PE_DIRECTION_VECTORS[pathDir]);
		}

		if (numNodes > 0) {
			foundPath.pathGoal = foundPath.path[0];
		}
	}

	return IPath::Ok;
}


/**
 * Try to read offset and vertices data from file, return false on failure
 */
bool CPathEstimator::ReadFile(const std::string& baseFileName, const std::string& mapName)
{
	const std::string hashHexString = IntToString(fileHashCode, "%x");
	const std::string cacheFileName = GetPathCacheDir() + mapName + "." + baseFileName + "-" + hashHexString + ".zip";

	LOG("[PathEstimator::%s] hash=%s file=\"%s\" (exists=%d)", __func__, hashHexString.c_str(), cacheFileName.c_str(), FileSystem::FileExists(cacheFileName));

	if (!FileSystem::FileExists(cacheFileName))
		return false;

	std::unique_ptr<IArchive> upfile(archiveLoader.OpenArchive(dataDirsAccess.LocateFile(cacheFileName), "sdz"));

	if (upfile == nullptr || !upfile->IsOpen()) {
		FileSystem::Remove(cacheFileName);
		return false;
	}

	char calcMsg[512];
	sprintf(calcMsg, "Reading Estimate PathCosts [%d]", BLOCK_SIZE);
	loadscreen->SetLoadMessage(calcMsg);

	const unsigned fid = upfile->FindFile("pathinfo");
	if (fid >= upfile->NumFiles()) {
		FileSystem::Remove(cacheFileName);
		return false;
	}

	// pointless; gets reassigned the output of CalcChecksum and CRC is not otherwise used
	// pathChecksum = upfile->GetCrc32(fid);

	std::vector<std::uint8_t> buffer;

	if (!upfile->GetFile(fid, buffer) || buffer.size() < 4) {
		FileSystem::Remove(cacheFileName);
		return false;
	}

	const unsigned int filehash = *(reinterpret_cast<unsigned int*>(&buffer[0]));
	const unsigned int blockSize = blockStates.GetSize() * sizeof(short2);
	unsigned int pos = sizeof(unsigned);

	if (filehash != fileHashCode) {
		FileSystem::Remove(cacheFileName);
		return false;
	}

	if (buffer.size() < (pos + blockSize * moveDefHandler->GetNumMoveDefs())) {
		FileSystem::Remove(cacheFileName);
		return false;
	}

	// read center-offset data
	for (int pathType = 0; pathType < moveDefHandler->GetNumMoveDefs(); ++pathType) {
		std::memcpy(&blockStates.peNodeOffsets[pathType][0], &buffer[pos], blockSize);
		pos += blockSize;
	}

	// read vertex-cost data
	if (buffer.size() < pos + vertexCosts.size() * sizeof(float)) {
		FileSystem::Remove(cacheFileName);
		return false;
	}

	std::memcpy(&vertexCosts[0], &buffer[pos], vertexCosts.size() * sizeof(float));
	return true;
}


/**
 * Try to write offset and vertex data to file.
 */
void CPathEstimator::WriteFile(const std::string& baseFileName, const std::string& mapName)
{
	// we need this directory to exist
	if (!FileSystem::CreateDirectory(GetPathCacheDir()))
		return;

	const std::string hashHexString = IntToString(fileHashCode, "%x");
	const std::string cacheFileName = GetPathCacheDir() + mapName + "." + baseFileName + "-" + hashHexString + ".zip";

	LOG("[PathEstimator::%s] hash=%s file=\"%s\" (exists=%d)", __func__, hashHexString.c_str(), cacheFileName.c_str(), FileSystem::FileExists(cacheFileName));

	// open file for writing in a suitable location
	zipFile file = zipOpen(dataDirsAccess.LocateFile(cacheFileName, FileQueryFlags::WRITE).c_str(), APPEND_STATUS_CREATE);

	if (file == nullptr)
		return;

	zipOpenNewFileInZip(file, "pathinfo", nullptr, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_BEST_COMPRESSION);

	// write hash-code (NOTE: this also affects the CRC!)
	zipWriteInFileInZip(file, (const void*) &fileHashCode, 4);

	// write center-offsets
	for (int pathType = 0; pathType < moveDefHandler->GetNumMoveDefs(); ++pathType) {
		zipWriteInFileInZip(file, (const void*) &blockStates.peNodeOffsets[pathType][0], blockStates.peNodeOffsets[pathType].size() * sizeof(short2));
	}

	// write vertex-costs
	zipWriteInFileInZip(file, vertexCosts.data(), vertexCosts.size() * sizeof(float));

	zipCloseFileInZip(file);
	zipClose(file, nullptr);


	// get the CRC over the written path data
	std::unique_ptr<IArchive> upfile(archiveLoader.OpenArchive(dataDirsAccess.LocateFile(cacheFileName), "sdz"));

	if (upfile == nullptr || !upfile->IsOpen()) {
		FileSystem::Remove(cacheFileName);
		return;
	}

	assert(upfile->FindFile("pathinfo") < upfile->NumFiles());
	// pointless; gets reassigned the output of CalcChecksum and CRC is not otherwise used
	// pathChecksum = upfile->GetCrc32(upfile->FindFile("pathinfo"));
}


std::uint32_t CPathEstimator::CalcChecksum() const
{
	std::uint32_t cs = 0;
	std::uint64_t nb = 0;

	#if (ENABLE_NETLOG_CHECKSUM == 1)
	std::array<char, 256> msgBuffer;
	std::array<uint8_t, sha512::SHA_LEN> shaBytes;
	std::vector<uint8_t> rawBytes;
	#endif

	for (const auto& pathTypeOffsets: blockStates.peNodeOffsets) {
		nb = pathTypeOffsets.size() * sizeof(short2);
		cs = HsiehHash(pathTypeOffsets.data(), nb, cs);

		#if (ENABLE_NETLOG_CHECKSUM == 1)
		rawBytes.resize(rawBytes.size() + nb, 0);
		std::memcpy(&rawBytes[rawBytes.size() - nb], pathTypeOffsets.data(), nb);
		#endif
	}

	nb = vertexCosts.size() * sizeof(float);
	cs = HsiehHash(vertexCosts.data(), nb, cs);

	#if (ENABLE_NETLOG_CHECKSUM == 1)
	{
		rawBytes.resize(rawBytes.size() + nb);

		std::memcpy(&rawBytes[rawBytes.size() - nb], vertexCosts.data(), nb);
		sha512::calc_digest(rawBytes, shaBytes);

		char* buf = msgBuffer.data();
		char* ptr = msgBuffer.data();

		ptr += snprintf(ptr, msgBuffer.size() - (ptr - buf), "[PE::%s][BLK_SIZE=%d][SHA512=", __func__, BLOCK_SIZE);

		for (uint8_t i = 0; i < sha512::SHA_LEN; i++) {
			ptr += snprintf(ptr, msgBuffer.size() - (ptr - buf), "%02x", shaBytes[i]);
		}

		ptr += snprintf(ptr, msgBuffer.size() - (ptr - buf), "]");

		CLIENT_NETLOG(gu->myPlayerNum, msgBuffer.data());
	}
	#endif

	return cs;
}


/**
 * Returns a hash-code identifying the dataset of this estimator.
 */
std::uint32_t CPathEstimator::CalcHash(const char* caller) const
{
	const unsigned int hmChecksum = readMap->CalcHeightmapChecksum();
	const unsigned int tmChecksum = readMap->CalcTypemapChecksum();
	const unsigned int mdChecksum = moveDefHandler->GetCheckSum();
	const unsigned int bmChecksum = groundBlockingObjectMap->CalcChecksum();
	const unsigned int peHashCode = (hmChecksum + tmChecksum + mdChecksum + bmChecksum + BLOCK_SIZE + PATHESTIMATOR_VERSION);

	LOG("[PathEstimator::%s][%s] BLOCK_SIZE=%u", __func__, caller, BLOCK_SIZE);
	LOG("[PathEstimator::%s][%s] PATHESTIMATOR_VERSION=%u", __func__, caller, PATHESTIMATOR_VERSION);
	LOG("[PathEstimator::%s][%s] heightMapChecksum=%x", __func__, caller, hmChecksum);
	LOG("[PathEstimator::%s][%s] typeMapChecksum=%x", __func__, caller, tmChecksum);
	LOG("[PathEstimator::%s][%s] moveDefChecksum=%x", __func__, caller, mdChecksum);
	LOG("[PathEstimator::%s][%s] blockMapChecksum=%x", __func__, caller, bmChecksum);
	LOG("[PathEstimator::%s][%s] estimatorHashCode=%x", __func__, caller, peHashCode);

	return peHashCode;
}
