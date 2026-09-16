#include "fastareader.h"
#include "util.h"
#include <htslib/bgzf.h>
#include <htslib/kseq.h>
#include <stdexcept>
#include <utility>

KSEQ_INIT(BGZF*, bgzf_read)

struct FastaReader::ReaderState {
    explicit ReaderState(const string& filename)
        : file(bgzf_open(filename.c_str(), "r")), sequence(nullptr)
    {
        if (!file)
            throw invalid_argument("Could not open reference FASTA: " + filename);
        sequence = kseq_init(file);
        if (!sequence) {
            bgzf_close(file);
            throw bad_alloc();
        }
    }

    ~ReaderState() {
        kseq_destroy(sequence);
        bgzf_close(file);
    }

    BGZF* file;
    kseq_t* sequence;
};

FastaReader::FastaReader(string faFile, bool forceUpperCase)
    : mFastaFile(std::move(faFile)),
      mForceUpperCase(forceUpperCase),
      mReachedEnd(false)
{
    if (is_directory(mFastaFile))
        throw invalid_argument("Reference FASTA is a directory: " + mFastaFile);
    mReader = std::make_unique<ReaderState>(mFastaFile);
}

FastaReader::~FastaReader() = default;

bool FastaReader::hasNext() {
    return !mReachedEnd;
}

void FastaReader::readNext() {
    if (mReachedEnd)
        return;

    const int status = kseq_read(mReader->sequence);
    if (status == -1) {
        mReachedEnd = true;
        mCurrentID.clear();
        mCurrentDescription.clear();
        mCurrentSequence.clear();
        return;
    }
    if (status < -1)
        throw runtime_error("Could not read reference FASTA: " + mFastaFile +
                            " (htslib status " + to_string(status) + ")");

    const auto* record = mReader->sequence;
    if (record->name.s)
        mCurrentID.assign(record->name.s, record->name.l);
    else
        mCurrentID.clear();
    if (record->comment.s)
        mCurrentDescription.assign(record->comment.s, record->comment.l);
    else
        mCurrentDescription.clear();
    if (record->seq.s)
        mCurrentSequence.assign(record->seq.s, record->seq.l);
    else
        mCurrentSequence.clear();
    str_keep_valid_sequence(mCurrentSequence, mForceUpperCase);
}

void FastaReader::readAll() {
    while (hasNext()) {
        readNext();
        if (!mReachedEnd)
            mAllContigs[mCurrentID] = mCurrentSequence;
    }
}

bool FastaReader::test(){
    FastaReader reader("testdata/tinyref.fa");
    reader.readAll();

    string contig1 = "GATCACAGGTCTATCACCCTATTAATTGGTATTTTCGTCTGGGGGGTGTGGAGCCGGAGCACCCTATGTCGCAGT";
    string contig2 = "GTCTGCACAGCCGCTTTCCACACAGAACCCCCCCCTCCCCCCGCTTCTGGCAAACCCCAAAAACAAAGAACCCTA";

    if(reader.mAllContigs.count("contig1") == 0 || reader.mAllContigs.count("contig2") == 0 )
        return false;

    if(reader.mAllContigs["contig1"] != contig1 || reader.mAllContigs["contig2"] != contig2 )
        return false;

    return true;

}



