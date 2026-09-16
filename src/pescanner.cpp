#include "pescanner.h"
#include "fastqreader.h"
#include <iostream>
#include "htmlreporter.h"
#include <stdexcept>
#include <thread>
#include <memory.h>
#include "util.h"
#include "jsonreporter.h"

PairEndScanner::PairEndScanner(string fusionFile, string refFile, string read1File, string read2File, string html, string json, int threadNum){
    mRead1File = read1File;
    mRead2File = read2File;
    mFusionFile = fusionFile;
    mRefFile = refFile;
    mHtmlFile = html;
    mJsonFile = json;
    mThreadNum = threadNum;
    mFusionMapper = NULL;
}

PairEndScanner::~PairEndScanner() {
    if(mFusionMapper != NULL) {
        delete mFusionMapper;
        mFusionMapper = NULL;
    }
}

bool PairEndScanner::scan(){
    if (mThreadNum < 1)
        throw std::invalid_argument("worker thread count must be positive");

    mQueues.clear();
    delete mFusionMapper;
    mFusionMapper = new FusionMapper(mRefFile, mFusionFile);
    mWorkerMatches.clear();
    mWorkerMatches.resize(mThreadNum);
    mNextQueue = 0;
    const uint32_t queueSize =
        static_cast<uint32_t>(PACK_IN_MEM_LIMIT / mThreadNum + 2);
    mQueues.reserve(mThreadNum);
    for (int t = 0; t < mThreadNum; ++t)
        mQueues.emplace_back(std::make_unique<ProducerConsumerQueue<ReadPairPack*>>(queueSize));

    std::vector<std::thread> workers;
    workers.reserve(mThreadNum);
    for (int t = 0; t < mThreadNum; ++t)
        workers.emplace_back(&PairEndScanner::consumerTask, this, static_cast<size_t>(t));
    std::thread producer(&PairEndScanner::producerTask, this);

    producer.join();
    for (auto& worker : workers)
        worker.join();

    // Each worker's matches already follow its packs in input order.
    // Merge the worker lists by producer-assigned pack number.
    std::vector<size_t> offsets(mWorkerMatches.size(), 0);
    while (true) {
        size_t selected = mWorkerMatches.size();
        for (size_t worker = 0; worker < mWorkerMatches.size(); ++worker) {
            const auto& matches = mWorkerMatches[worker];
            if (offsets[worker] == matches.size())
                continue;
            if (selected == mWorkerMatches.size() ||
                matches[offsets[worker]].packOrdinal <
                    mWorkerMatches[selected][offsets[selected]].packOrdinal)
                selected = worker;
        }
        if (selected == mWorkerMatches.size())
            break;
        mFusionMapper->addMatch(
            mWorkerMatches[selected][offsets[selected]++].match);
    }
    for (auto& workerMatches : mWorkerMatches)
        std::vector<OrderedMatch>().swap(workerMatches);

    mFusionMapper->filterMatches();
    mFusionMapper->sortMatches();
    mFusionMapper->clusterMatches();

    htmlReport();
    jsonReport();

    mFusionMapper->freeMatches();
    return true;
}

bool PairEndScanner::scanPairEnd(ReadPairPack* pack, std::vector<OrderedMatch>& workerMatches){
    for(int p=0;p<pack->count;p++){
        ReadPair* pair = pack->data[p];
        Read* r1 = pair->mLeft;
        Read* r2 = pair->mRight;
        Read* rcr1 = NULL;
        Read* rcr2 = NULL;
        Read* merged = pair->fastMerge();
        Read* mergedRC = NULL;
        bool mapable = false;
        // if merged successfully, we only search the merged
        if(merged != NULL) {
            Match* matchMerged = mFusionMapper->mapRead(merged, mapable);
            if(matchMerged){
                matchMerged->addOriginalPair(pair);
                workerMatches.push_back({pack->ordinal, matchMerged});
            } else if(mapable){
                mergedRC = merged->reverseComplement();
                Match* matchMergedRC = mFusionMapper->mapRead(mergedRC, mapable);
                if(matchMergedRC){
                    matchMergedRC->addOriginalPair(pair);
                    workerMatches.push_back({pack->ordinal, matchMergedRC});
                }
                delete mergedRC;
            }

            delete pair;
            delete merged;
            continue;
        }
        // else still search R1 and R2 separatedly
        mapable = false;
        Match* matchR1 = mFusionMapper->mapRead(r1, mapable);
        if(matchR1){
            matchR1->addOriginalPair(pair);
            workerMatches.push_back({pack->ordinal, matchR1});
        } else if(mapable){
            rcr1 = r1->reverseComplement();
            Match* matchRcr1 = mFusionMapper->mapRead(rcr1, mapable);
            if(matchRcr1){
                matchRcr1->addOriginalPair(pair);
                matchRcr1->setReversed(true);
                workerMatches.push_back({pack->ordinal, matchRcr1});
            }
            delete rcr1;
        }
        mapable = false;
        Match* matchR2 = mFusionMapper->mapRead(r2, mapable);
        if(matchR2){
            matchR2->addOriginalPair(pair);
            workerMatches.push_back({pack->ordinal, matchR2});
        } else if(mapable) {
            rcr2 = r2->reverseComplement();
            Match* matchRcr2 = mFusionMapper->mapRead(rcr2, mapable);
            if(matchRcr2){
                matchRcr2->addOriginalPair(pair);
                matchRcr2->setReversed(true);
                workerMatches.push_back({pack->ordinal, matchRcr2});
            }
            delete rcr2;
        }
        delete pair;
    }

    delete[] pack->data;
    delete pack;

    return true;
}

void PairEndScanner::producePack(ReadPairPack* pack){
    // Try each dedicated SPSC queue before waiting for a full one.
    const size_t queueCount = mQueues.size();
    for (size_t attempt = 0; attempt < queueCount; ++attempt) {
        const size_t index = (mNextQueue + attempt) % queueCount;
        if (mQueues[index]->write(pack)) {
            mNextQueue = (index + 1) % queueCount;
            return;
        }
    }
    mQueues[mNextQueue]->writeBlocking(pack);
    mNextQueue = (mNextQueue + 1) % queueCount;
}

void PairEndScanner::producerTask()
{
    ReadPair** data = new ReadPair*[PACK_SIZE]();
    FastqReaderPair reader(mRead1File, mRead2File);
    int count = 0;
    size_t packOrdinal = 0;
    while (true) {
        ReadPair* read = reader.read();
        if (!read) {
            producePack(new ReadPairPack{data, count, packOrdinal++});
            break;
        }
        data[count++] = read;
        if (count == PACK_SIZE) {
            producePack(new ReadPairPack{data, count, packOrdinal++});
            data = new ReadPair*[PACK_SIZE]();
            count = 0;
        }
    }

    // A terminal record follows all data in each worker's queue.
    for (auto& queue : mQueues)
        queue->writeBlocking(static_cast<ReadPairPack*>(nullptr));
}

void PairEndScanner::consumerTask(size_t queueIndex)
{
    ReadPairPack* pack = nullptr;
    auto& queue = *mQueues[queueIndex];
    while (true) {
        queue.readBlocking(pack);
        if (!pack)
            break;
        scanPairEnd(pack, mWorkerMatches[queueIndex]);
    }
}

void PairEndScanner::textReport() {
}

void PairEndScanner::htmlReport() {
    if(mHtmlFile == "")
        return;

    HtmlReporter reporter(mHtmlFile, mFusionMapper);
    reporter.run();
}

void PairEndScanner::jsonReport() {
    if(mJsonFile == "")
        return;

    JsonReporter reporter(mJsonFile, mFusionMapper);
    reporter.run();
}
