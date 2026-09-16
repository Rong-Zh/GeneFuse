#ifndef PE_SCANNNER_H
#define PE_SCANNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "read.h"
#include "fusion.h"
#include "match.h"
#include <cstdlib>
#include "ProducerConsumerQueue.h"
#include <cstddef>
#include <memory>
#include <vector>
#include <thread>
#include "fusionmapper.h"


using namespace std;

struct ReadPairPack {
    ReadPair** data;
    int count;
    size_t ordinal;
};

typedef struct ReadPairPack ReadPairPack;

class PairEndScanner{
public:
    PairEndScanner(string fusionFile, string refFile, string read1File, string read2File, string html, string json, int threadnum);
    ~PairEndScanner();
    bool scan();
    void textReport();
    void htmlReport();
    void jsonReport();

private:
    struct OrderedMatch {
        size_t packOrdinal;
        Match* match;
    };
    bool scanPairEnd(ReadPairPack* pack, std::vector<OrderedMatch>& workerMatches);
    void producePack(ReadPairPack* pack);
    void producerTask();
    void consumerTask(size_t queueIndex);

private:
    string mFusionFile;
    string mRefFile;
    string mRead1File;
    string mRead2File;
    string mHtmlFile;
    string mJsonFile;
    std::vector<std::unique_ptr<ProducerConsumerQueue<ReadPairPack*>>> mQueues;
    size_t mNextQueue;
    std::vector<std::vector<OrderedMatch>> mWorkerMatches;
    int mThreadNum;
    FusionMapper* mFusionMapper;
};


#endif