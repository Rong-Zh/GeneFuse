#ifndef FASTA_READER_H
#define FASTA_READER_H

#include <map>
#include <memory>
#include <string>

using namespace std;

class FastaReader
{
public:
    FastaReader(string fastaFile, bool forceUpperCase = true);
    ~FastaReader();
    bool hasNext();
    void readNext();
    void readAll();

    inline string currentID() { return mCurrentID; }
    inline string currentDescription() { return mCurrentDescription; }
    inline string currentSequence() { return mCurrentSequence; }
    inline map<string, string>& contigs() { return mAllContigs; }

    static bool test();

public:
    string mCurrentSequence;
    string mCurrentID;
    string mCurrentDescription;
    map<string, string> mAllContigs;

private:
    struct ReaderState;
    string mFastaFile;
    std::unique_ptr<ReaderState> mReader;
    bool mForceUpperCase;
    bool mReachedEnd;
};

#endif
