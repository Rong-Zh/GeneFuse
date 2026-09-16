#include "sescanner.h"
#include "fastqreader.h"
#include <iostream>
#include "htmlreporter.h"
#include <stdexcept>
#include <thread>
#include <memory.h>
#include "util.h"
#include "jsonreporter.h"

SingleEndScanner::SingleEndScanner(string fusionFile, string refFile, string read1File, string html, string json, int threadNum){
    mRead1File = read1File;
    mFusionFile = fusionFile;
    mRefFile = refFile;
    mHtmlFile = html;
    mJsonFile = json;
    mThreadNum = threadNum;
    mFusionMapper = NULL;
}

SingleEndScanner::~SingleEndScanner() {
    if(mFusionMapper != NULL) {
        delete mFusionMapper;
        mFusionMapper = NULL;
    }
}

bool SingleEndScanner::scan(){
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
        mQueues.emplace_back(std::make_unique<ProducerConsumerQueue<ReadPack*>>(queueSize));

    std::vector<std::thread> workers;
    workers.reserve(mThreadNum);
    for (int t = 0; t < mThreadNum; ++t)
        workers.emplace_back(&SingleEndScanner::consumerTask, this, static_cast<size_t>(t));
    std::thread producer(&SingleEndScanner::producerTask, this);

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

bool SingleEndScanner::scanSingleEnd(ReadPack* pack, std::vector<OrderedMatch>& workerMatches){
    for(int p=0;p<pack->count;p++){
        Read* r1 = pack->data[p];
        bool mapable = false;
        Match* matchR1 = mFusionMapper->mapRead(r1, mapable);
        if(matchR1){
            matchR1->addOriginalRead(r1);
            workerMatches.push_back({pack->ordinal, matchR1});
        } else if(mapable){
            Read* rcr1 = r1->reverseComplement();
            Match* matchRcr1 = mFusionMapper->mapRead(rcr1, mapable);
            if(matchRcr1){
                matchRcr1->addOriginalRead(r1);
                matchRcr1->setReversed(true);
                workerMatches.push_back({pack->ordinal, matchRcr1});
            }
            delete rcr1;
        }
        delete r1;
    }

    delete[] pack->data;
    delete pack;

    return true;
}

void SingleEndScanner::producePack(ReadPack* pack){
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

void SingleEndScanner::producerTask()
{
    Read** data = new Read*[PACK_SIZE]();
    FastqReader reader(mRead1File);
    int count = 0;
    size_t packOrdinal = 0;
    while (true) {
        Read* read = reader.read();
        if (!read) {
            producePack(new ReadPack{data, count, packOrdinal++});
            break;
        }
        data[count++] = read;
        if (count == PACK_SIZE) {
            producePack(new ReadPack{data, count, packOrdinal++});
            data = new Read*[PACK_SIZE]();
            count = 0;
        }
    }

    // A terminal record follows all data in each worker's queue.
    for (auto& queue : mQueues)
        queue->writeBlocking(static_cast<ReadPack*>(nullptr));
}

void SingleEndScanner::consumerTask(size_t queueIndex)
{
    ReadPack* pack = nullptr;
    auto& queue = *mQueues[queueIndex];
    while (true) {
        queue.readBlocking(pack);
        if (!pack)
            break;
        scanSingleEnd(pack, mWorkerMatches[queueIndex]);
    }
}

void SingleEndScanner::textReport() {
}

void SingleEndScanner::htmlReport() {
    if(mHtmlFile == "")
        return;

    HtmlReporter reporter(mHtmlFile, mFusionMapper);
    reporter.run();
}

void SingleEndScanner::jsonReport() {
    if(mJsonFile == "")
        return;

    JsonReporter reporter(mJsonFile, mFusionMapper);
    reporter.run();
}
