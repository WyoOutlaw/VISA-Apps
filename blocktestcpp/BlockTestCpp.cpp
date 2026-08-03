// BlockTestCpp.cpp
//
// Native C++, VISA-only rewrite of Source\mtests\BlockTest (the C# WinForms tool).
// Sweeps write ("poke") and read ("peek") block sizes against a VISA instrument,
// times each transfer, and always writes a CSV of the results.
//
// Wire protocol (must match the instrument-side SCPI parser used by the original
// tool -- see BlockTest.cs write path ~2892-2947, read path ~2965-3048):
//   Poke:  "W? #<numDigits><length>" + <length bytes of payload> + "\n"
//          expect response "<length>\n" or "+<length>\n"
//   Peek:  "R? <length>\n"
//          expect response "#<numDigits><length>" + <length bytes of data> + "\n"

#define NOMINMAX
#include <windows.h>
#include <visa.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

struct Options
{
    std::string address = "TCPIP0::127.0.0.1::hislip0::INSTR";
    long initialSize = 500;
    long finalSize = 130000;
    long increment = 500;
    double multiplier = 1.0;
    long samplesPerSize = 1;
    std::string csvPath = "BlockTest.csv";
    bool verbose = false;
};

struct SizeStats
{
    long length = 0;
    double minWriteTime = 0, aveWriteTime = 0, maxWriteTime = 0;
    double minReadTime = 0, aveReadTime = 0, maxReadTime = 0;
    double writeBytesPerSec = 0, readBytesPerSec = 0;
};

const ViUInt32 kTimeoutMs = 10000;
const size_t kFramingOverhead = 64; // room for "#<digits><digits>"/"W? #.."/"\n" framing beyond the payload

void PrintUsage(const char* exeName)
{
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  -a <address>   VISA resource string (default TCPIP0::127.0.0.1::hislip0::INSTR)\n"
        "                 or e.g. TCPIP0::host::5025::SOCKET for a raw-socket VISA session\n"
        "  -i <bytes>     initial block size (default 500)\n"
        "  -f <bytes>     final block size (default 130000)\n"
        "  -c <bytes>     size increment (default 500)\n"
        "  -m <factor>    size multiplier (default 1)\n"
        "  -n <count>     samples per block size, used for min/ave/max (default 1)\n"
        "  -csv <path>    CSV output file, always written (default BlockTest.csv)\n"
        "  -v             verbose: print each individual write/read sample timing\n"
        "  -h, -?         show this help\n",
        exeName);
}

const char* NextArgValue(int argc, char** argv, int& i, const char* flag)
{
    if (i + 1 >= argc)
    {
        std::fprintf(stderr, "Error: %s requires a value\n", flag);
        std::exit(2);
    }
    return argv[++i];
}

bool ParseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "-a") opt.address = NextArgValue(argc, argv, i, "-a");
        else if (arg == "-i") opt.initialSize = std::atol(NextArgValue(argc, argv, i, "-i"));
        else if (arg == "-f") opt.finalSize = std::atol(NextArgValue(argc, argv, i, "-f"));
        else if (arg == "-c") opt.increment = std::atol(NextArgValue(argc, argv, i, "-c"));
        else if (arg == "-m") opt.multiplier = std::atof(NextArgValue(argc, argv, i, "-m"));
        else if (arg == "-n") opt.samplesPerSize = std::atol(NextArgValue(argc, argv, i, "-n"));
        else if (arg == "-csv") opt.csvPath = NextArgValue(argc, argv, i, "-csv");
        else if (arg == "-v") opt.verbose = true;
        else if (arg == "-h" || arg == "-?" || arg == "--help")
        {
            PrintUsage(argv[0]);
            std::exit(0);
        }
        else
        {
            std::fprintf(stderr, "Error: unknown argument '%s'\n", arg.c_str());
            return false;
        }
    }

    if (opt.finalSize < opt.initialSize)
    {
        std::fprintf(stderr, "Error: final size (%ld) must be >= initial size (%ld)\n", opt.finalSize, opt.initialSize);
        return false;
    }
    if (opt.samplesPerSize < 1)
    {
        std::fprintf(stderr, "Error: -n must be at least 1\n");
        return false;
    }
    return true;
}

// Matches BlockTest.cs:2451-2469 exactly (condition-before-body, increment-at-end-of-iteration
// for-loop semantics, and the lastLength guard against a stalled step size).
std::vector<long> ComputeBlockSizes(const Options& opt)
{
    std::vector<long> sizes;
    long lastLength = opt.initialSize - 1;
    for (long length = opt.initialSize; length <= opt.finalSize;
         length = (long)((double)length * opt.multiplier + opt.increment))
    {
        if (length == lastLength) length++;
        if (length > opt.finalSize) break;
        sizes.push_back(length);
        lastLength = length;
    }
    return sizes;
}

class VisaError : public std::runtime_error
{
public:
    explicit VisaError(const std::string& msg) : std::runtime_error(msg) {}
};

void CheckStatus(ViSession session, ViStatus status, const char* what)
{
    if (status < VI_SUCCESS)
    {
        ViChar desc[256] = {0};
        viStatusDesc(session, status, desc);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s failed (0x%08X): %s", what, (unsigned)status, desc);
        throw VisaError(buf);
    }
}

void FillPattern(unsigned char* dst, long length)
{
    for (long i = 0; i < length; i++)
        dst[i] = (unsigned char)('A' + (i % 26));
}

// Console-friendly auto-scaled throughput (B/s, KB/s, or MB/s); the CSV keeps raw
// bytes/sec (matching the original tool's columns) since that's more useful for
// spreadsheet analysis than a mixed-unit column.
std::string FormatBytesPerSec(double bytesPerSec)
{
    const double kKB = 1024.0;
    const double kMB = 1024.0 * 1024.0;
    char buf[64];
    if (bytesPerSec >= kMB)
        std::snprintf(buf, sizeof(buf), "%.2f MB/s", bytesPerSec / kMB);
    else if (bytesPerSec >= kKB)
        std::snprintf(buf, sizeof(buf), "%.2f KB/s", bytesPerSec / kKB);
    else
        std::snprintf(buf, sizeof(buf), "%.2f B/s", bytesPerSec);
    return buf;
}

class VisaBlockTester
{
public:
    VisaBlockTester(const std::string& address, long maxBlockSize)
    {
        ViStatus status = viOpenDefaultRM(&m_rm);
        CheckStatus(VI_NULL, status, "viOpenDefaultRM");

        status = viOpen(m_rm, const_cast<char*>(address.c_str()), VI_NULL, VI_NULL, &m_session);
        CheckStatus(m_rm, status, "viOpen");

        status = viSetAttribute(m_session, VI_ATTR_TMO_VALUE, kTimeoutMs);
        CheckStatus(m_session, status, "viSetAttribute(VI_ATTR_TMO_VALUE)");

        m_buffer.resize((size_t)maxBlockSize + kFramingOverhead);
        m_readBuffer.resize((size_t)maxBlockSize + kFramingOverhead);
    }

    ~VisaBlockTester()
    {
        if (m_session != VI_NULL) viClose(m_session);
        if (m_rm != VI_NULL) viClose(m_rm);
    }

    // "W? #<numDigits><length>" + payload + "\n", timed as one write+read span
    // (matches IoTypeVisa.cs Query(), which times viWrite+viRead together, not separately).
    double TimedPoke(long length)
    {
        std::string header = "W? #" + std::to_string(std::to_string(length).size()) + std::to_string(length);
        size_t pos = 0;
        std::memcpy(&m_buffer[pos], header.data(), header.size());
        pos += header.size();
        FillPattern(&m_buffer[pos], length);
        pos += (size_t)length;
        m_buffer[pos++] = 0x0A;

        ViUInt32 readCount = 0;
        double elapsed = TimedWriteThenRead(m_buffer.data(), (ViUInt32)pos, m_readBuffer.data(), (ViUInt32)m_readBuffer.size(), readCount);

        std::string expected1 = std::to_string(length) + "\n";
        std::string expected2 = "+" + std::to_string(length) + "\n";
        std::string actual((char*)m_readBuffer.data(), readCount);
        if (actual != expected1 && actual != expected2)
            throw VisaError("Unexpected write-block response, expected '" + expected1 + "' got '" + actual + "'");

        return elapsed;
    }

    // "R? <length>\n", expecting "#<numDigits><length><data>\n" back, timed as one write+read span.
    double TimedPeek(long length)
    {
        std::string cmd = "R? " + std::to_string(length) + "\n";
        std::memcpy(m_buffer.data(), cmd.data(), cmd.size());

        size_t numDigits = std::to_string(length).size();
        std::string headerExpected = "#" + std::to_string(numDigits) + std::to_string(length);
        size_t expectedTotal = headerExpected.size() + (size_t)length + 1; // + trailing '\n'

        ViUInt32 readCount = 0;
        double elapsed = TimedWriteThenRead(m_buffer.data(), (ViUInt32)cmd.size(), m_readBuffer.data(), (ViUInt32)m_readBuffer.size(), readCount);

        std::string headerActual((char*)m_readBuffer.data(), headerExpected.size());
        if (headerActual != headerExpected)
            throw VisaError("Unexpected read-block response header, expected '" + headerExpected + "' got '" + headerActual + "'");
        if (readCount < expectedTotal || m_readBuffer[expectedTotal - 1] != '\n')
            throw VisaError("Read-block response missing trailing newline or was short");

        return elapsed;
    }

private:
    double TimedWriteThenRead(const void* writeData, ViUInt32 writeLen, void* readBuf, ViUInt32 readBufSize, ViUInt32& actualRead)
    {
        LARGE_INTEGER t1, t2, freq;
        QueryPerformanceFrequency(&freq);
        ViUInt32 writeActual = 0;

        QueryPerformanceCounter(&t1);
        ViStatus status = viWrite(m_session, (ViBuf)writeData, writeLen, &writeActual);
        CheckStatus(m_session, status, "viWrite");
        if (writeActual != writeLen)
            throw VisaError("viWrite wrote fewer bytes than requested");

        status = viRead(m_session, (ViPBuf)readBuf, readBufSize, &actualRead);
        QueryPerformanceCounter(&t2);
        CheckStatus(m_session, status, "viRead");

        return (double)(t2.QuadPart - t1.QuadPart) / (double)freq.QuadPart;
    }

    ViSession m_rm = VI_NULL;
    ViSession m_session = VI_NULL;
    std::vector<unsigned char> m_buffer;
    std::vector<unsigned char> m_readBuffer;
};

SizeStats RunOneSize(VisaBlockTester& tester, long length, long samplesPerSize, bool verbose)
{
    SizeStats s;
    s.length = length;

    double writeTotal = 0, readTotal = 0;
    double writeMin = std::numeric_limits<double>::max(), writeMax = 0;
    double readMin = std::numeric_limits<double>::max(), readMax = 0;

    for (long sample = 0; sample < samplesPerSize; sample++)
    {
        double writeTime = tester.TimedPoke(length);
        writeTotal += writeTime;
        if (writeTime < writeMin) writeMin = writeTime;
        if (writeTime > writeMax) writeMax = writeTime;

        double readTime = tester.TimedPeek(length);
        readTotal += readTime;
        if (readTime < readMin) readMin = readTime;
        if (readTime > readMax) readMax = readTime;

        if (verbose)
        {
            double writeBps = writeTime > 0 ? (double)length / writeTime : 0.0;
            double readBps = readTime > 0 ? (double)length / readTime : 0.0;
            std::printf("    [%ld/%ld] write=%.6f s (%s)  read=%.6f s (%s)\n",
                sample + 1, samplesPerSize,
                writeTime, FormatBytesPerSec(writeBps).c_str(),
                readTime, FormatBytesPerSec(readBps).c_str());
        }
    }

    s.aveWriteTime = writeTotal / samplesPerSize;
    s.minWriteTime = writeMin;
    s.maxWriteTime = writeMax;
    s.aveReadTime = readTotal / samplesPerSize;
    s.minReadTime = readMin;
    s.maxReadTime = readMax;
    s.writeBytesPerSec = s.aveWriteTime > 0 ? (double)length / s.aveWriteTime : 0.0;
    s.readBytesPerSec = s.aveReadTime > 0 ? (double)length / s.aveReadTime : 0.0;
    return s;
}

// Column layout matches BlockTest.cs:3387,3473-3477 (metadata rows simplified to
// plain Key,Value pairs -- the CPU/RAM metadata rows are dropped since those came
// from a separate native helper DLL not worth pulling in for this tool).
void WriteCsv(const Options& opt, const std::vector<SizeStats>& results)
{
    std::ofstream csv(opt.csvPath);
    if (!csv)
        throw std::runtime_error("Could not open CSV file for writing: " + opt.csvPath);

    std::time_t now = std::time(nullptr);
    char timeBuf[64] = {0};
    std::tm tmBuf;
    localtime_s(&tmBuf, &now);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);

    csv << "VISA Address," << opt.address << "\n";
    csv << "Start Time," << timeBuf << "\n";
    csv << "Initial Size," << opt.initialSize << "\n";
    csv << "Final Size," << opt.finalSize << "\n";
    csv << "Increment," << opt.increment << "\n";
    csv << "Multiplier," << opt.multiplier << "\n";
    csv << "Samples Per Size," << opt.samplesPerSize << "\n";
    csv << "\n";
    csv << "Block size (Bytes),,Average Write Speed (Bytes/sec),Average Read Speed (Bytes/sec),,"
           "Minimum Write Time (Seconds),Average Write Time (Seconds),Maximum Write Time (Seconds),,"
           "Minimum Read Time (Seconds),Average Read Time (Seconds),Maximum Read Time (Seconds)\n";

    for (const auto& s : results)
    {
        csv << s.length << ",,"
            << s.writeBytesPerSec << "," << s.readBytesPerSec << ",,"
            << s.minWriteTime << "," << s.aveWriteTime << "," << s.maxWriteTime << ",,"
            << s.minReadTime << "," << s.aveReadTime << "," << s.maxReadTime << "\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!ParseArgs(argc, argv, opt))
    {
        PrintUsage(argv[0]);
        return 2;
    }

    std::vector<long> sizes = ComputeBlockSizes(opt);
    if (sizes.empty())
    {
        std::fprintf(stderr, "Error: no block sizes to test (check -i/-f/-c/-m)\n");
        return 2;
    }

    std::vector<SizeStats> results;
    int exitCode = 0;

    try
    {
        VisaBlockTester tester(opt.address, opt.finalSize);
        std::printf("Testing %s: %zu block size(s) from %ld to %ld bytes, %ld sample(s) each\n",
            opt.address.c_str(), sizes.size(), sizes.front(), sizes.back(), opt.samplesPerSize);

        for (long length : sizes)
        {
            if (opt.verbose)
                std::printf("  %7ld bytes:\n", length);
            SizeStats s = RunOneSize(tester, length, opt.samplesPerSize, opt.verbose);
            std::printf("  %7ld bytes: write=%s  read=%s\n", length,
                FormatBytesPerSec(s.writeBytesPerSec).c_str(), FormatBytesPerSec(s.readBytesPerSec).c_str());
            results.push_back(s);
        }
    }
    catch (const std::exception& ex)
    {
        std::fprintf(stderr, "Error: %s\n", ex.what());
        exitCode = 1;
    }

    // Always write the CSV, even on failure, with whatever results were collected so far.
    try
    {
        WriteCsv(opt, results);
        std::printf("Results written to %s\n", opt.csvPath.c_str());
    }
    catch (const std::exception& ex)
    {
        std::fprintf(stderr, "Error writing CSV: %s\n", ex.what());
        exitCode = 1;
    }

    return exitCode;
}
