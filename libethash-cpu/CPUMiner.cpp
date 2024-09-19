/*
This file is part of ethminer.

ethminer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ethminer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 CPUMiner simulates mining devices but does NOT real mine!
 USE FOR DEVELOPMENT ONLY !
*/

#if defined(__linux__)
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* we need sched_setaffinity() */
#endif
#include <error.h>
#include <sched.h>
#include <unistd.h>
#endif

#include <libethcore/Farm.h>
#include <ethash/ethash.hpp>

#include <boost/version.hpp>

#if 0
#include <boost/fiber/numa/pin_thread.hpp>
#include <boost/fiber/numa/topology.hpp>
#endif

#include "CPUMiner.h"


/* Sanity check for defined OS */
#if defined(__APPLE__) || defined(__MACOSX)
/* MACOSX */
#include <mach/mach.h>
#elif defined(__linux__)
/* linux */
#elif defined(_WIN32)
/* windows */
#else
#error "Invalid OS configuration"
#endif


using namespace std;
using namespace dev;
using namespace eth;

// misc
bool memis0(const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (((const uint8_t*)p)[i])
            return false;
    return true;
}

void memxor(uint8_t* pDst, const uint8_t* pSrc, size_t n)
{
    for (size_t i = 0; i < n; i++)
        pDst[i] ^= pSrc[i];
}

char ChFromHex(uint8_t v)
{
    return v + ((v < 10) ? '0' : ('a' - 10));
}

void uintBigImpl::_Print(const uint8_t* pDst, uint32_t nDst, std::ostream& s)
{
    // const uint32_t nDigitsMax = 8;
    const uint32_t nDigitsMax = 64;
    if (nDst > nDigitsMax)
        nDst = nDigitsMax;  // truncate

    char sz[nDigitsMax * 2 + 1];

    _Print(pDst, nDst, sz);
    s << sz;
}

void uintBigImpl::_Print(const uint8_t* pDst, uint32_t nDst, char* sz)
{
    for (uint32_t i = 0; i < nDst; i++)
    {
        sz[i * 2] = ChFromHex(pDst[i] >> 4);
        sz[i * 2 + 1] = ChFromHex(pDst[i] & 0xf);
    }

    sz[nDst << 1] = 0;
}

void uintBigImpl::_Assign(uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc, uint32_t nSrc)
{
    if (nSrc >= nDst)
        memcpy(pDst, pSrc + nSrc - nDst, nDst);
    else
    {
        memset0(pDst, nDst - nSrc);
        memcpy(pDst + nDst - nSrc, pSrc, nSrc);
    }
}

uint8_t uintBigImpl::_Inc(uint8_t* pDst, uint32_t nDst)
{
    for (uint32_t i = nDst; i--;)
        if (++pDst[i])
            return 0;

    return 1;
}

uint8_t uintBigImpl::_Inc(uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc)
{
    uint16_t carry = 0;
    for (uint32_t i = nDst; i--;)
    {
        carry += pDst[i];
        carry += pSrc[i];

        pDst[i] = (uint8_t)carry;
        carry >>= 8;
    }

    return (uint8_t)carry;
}

uint8_t uintBigImpl::_Inc(uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc, uint32_t nSrc)
{
    if (nDst <= nSrc)
        return _Inc(pDst, nDst, pSrc + nSrc - nDst);  // src is at least our size

    if (!_Inc(pDst + nDst - nSrc, nSrc, pSrc))
        return 0;

    // propagete carry
    return _Inc(pDst, nDst - nSrc);
}

void uintBigImpl::_Inv(uint8_t* pDst, uint32_t nDst)
{
    for (uint32_t i = nDst; i--;)
        pDst[i] ^= 0xff;
}

void uintBigImpl::_Xor(uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc)
{
    for (uint32_t i = nDst; i--;)
        pDst[i] ^= pSrc[i];
}

void uintBigImpl::_Xor(uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc, uint32_t nSrc)
{
    if (nDst <= nSrc)
        _Xor(pDst, nDst, pSrc + nSrc - nDst);  // src is at least our size
    else
        _Xor(pDst + nDst - nSrc, nSrc, pSrc);
}

void uintBigImpl::_Mul(uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc0, uint32_t nSrc0,
    const uint8_t* pSrc1, uint32_t nSrc1)
{
    memset0(pDst, nDst);

    if (nSrc0 > nDst)
    {
        pSrc0 += nSrc0 - nDst;
        nSrc0 = nDst;
    }

    if (nSrc1 > nDst)
    {
        pSrc1 += nSrc1 - nDst;
        nSrc1 = nDst;
    }

    int32_t nDelta = nSrc0 + nSrc1 - nDst - 1;

    for (uint32_t i0 = nSrc0; i0--;)
    {
        uint8_t x0 = pSrc0[i0];
        uint16_t carry = 0;

        uint32_t iDst = i0 - nDelta;  // don't care if overflows
        uint32_t i1Min = (iDst > nDst) ? (-static_cast<int32_t>(iDst)) : 0;
        for (uint32_t i1 = nSrc1; i1-- > i1Min;)
        {
            uint8_t& dst = pDst[iDst + i1];

            uint16_t x1 = pSrc1[i1];
            x1 *= x0;
            carry += x1;
            carry += dst;

            dst = (uint8_t)carry;
            carry >>= 8;
        }

        if (iDst <= nDst)
            while (carry && iDst--)
            {
                uint8_t& dst = pDst[iDst];
                carry += dst;

                dst = (uint8_t)carry;
                carry >>= 8;
            }
    }
}

int uintBigImpl::_Cmp(const uint8_t* pSrc0, uint32_t nSrc0, const uint8_t* pSrc1, uint32_t nSrc1)
{
    if (nSrc0 > nSrc1)
    {
        uint32_t diff = nSrc0 - nSrc1;
        if (!memis0(pSrc0, diff))
            return 1;

        pSrc0 += diff;
        nSrc0 = nSrc1;
    }
    else if (nSrc0 < nSrc1)
    {
        uint32_t diff = nSrc1 - nSrc0;
        if (!memis0(pSrc1, diff))
            return -1;

        pSrc1 += diff;
    }

    return memcmp(pSrc0, pSrc1, nSrc0);
}

uint32_t uintBigImpl::_GetOrder(const uint8_t* pDst, uint32_t nDst)
{
    for (uint32_t nByte = 0;; nByte++)
    {
        if (nDst == nByte)
            return 0;  // the number is zero

        uint8_t x = pDst[nByte];
        if (!x)
            continue;

        uint32_t nOrder = ((nDst - nByte) << 3) - 7;
        while (x >>= 1)
            nOrder++;

        return nOrder;
    }
}

bool uintBigImpl::_Accept(uint8_t* pDst, const uint8_t* pThr, uint32_t nDst, uint32_t nThrOrder)
{
    if (!nThrOrder)
        return false;

    nThrOrder--;
    uint32_t nOffs = nDst - 1 - (nThrOrder >> 3);
    uint8_t msk = uint8_t(2 << (7 & nThrOrder)) - 1;
    assert(msk);

    pDst[nOffs] &= msk;

    if (memcmp(pDst + nOffs, pThr + nOffs, nDst - nOffs) >= 0)
        return false;

    memset0(pDst, nOffs);
    return true;
}

FourCC::Text::Text(uint32_t n)
{
    reinterpret_cast<uintBigFor<uint32_t>::Type&>(m_sz) = n;  // convertion
    m_sz[_countof(m_sz) - 1] = 0;

    // fix illegal chars
    for (size_t i = 0; i < _countof(m_sz) - 1; i++)
    {
        char& c = m_sz[i];
        if ((c < ' ') || (c > '~'))
            c = ' ';
    }
}

//std::ostream& operator<<(std::ostream& s, const FourCC& x)
//{
//    s << FourCC::Text(x);
//    return s;
//}
//
//std::ostream& operator<<(std::ostream& s, const FourCC::Text& x)
//{
//    s << x.m_sz;
//    return s;
//}

void uintBigImpl::_ShiftRight(
    uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc, uint32_t nSrc, uint32_t nBits)
{
    // assuming pDst and pSrc may be the same

    uint32_t nBytes = nBits >> 3;
    if (nBytes >= nSrc)
        nSrc = nBits = 0;
    else
    {
        nSrc -= nBytes;
        nBits &= 7;
    }

    uint8_t* pDst0 = pDst;

    if (nDst > nSrc)
    {
        pDst += nDst - nSrc;
        nDst = nSrc;
    }
    else
        pSrc += nSrc - nDst;

    if (nBits)
    {
        uint32_t nLShift = 8 - nBits;

        for (uint32_t i = nDst; i--;)
        {
            // pSrc and pDst may be the same
            pDst[i] = pSrc[i] >> nBits;
            if (nSrc + i > nDst)
                pDst[i] |= (pSrc[int32_t(i - 1)] << nLShift);
        }
    }
    else
        memmove(pDst, pSrc, nDst);

    memset0(pDst0, pDst - pDst0);
}

void uintBigImpl::_ShiftLeft(
    uint8_t* pDst, uint32_t nDst, const uint8_t* pSrc, uint32_t nSrc, uint32_t nBits)
{
    // assuming pDst and pSrc may be the same

    uint32_t nBytes = nBits >> 3;
    if (nBytes >= nDst)
    {
        nBytes = nDst;
        nDst = nBits = 0;
    }
    else
    {
        nBits &= 7;
        nDst -= nBytes;
    }

    uint8_t* pDst0 = pDst;

    if (nSrc > nDst)
    {
        pSrc += nSrc - nDst;
        nSrc = nDst;
    }
    else
    {
        memset0(pDst, nDst - nSrc);
        pDst += nDst - nSrc;
    }

    if (nBits)
    {
        if (nSrc)
        {
            uint32_t nRShift = 8 - nBits;

            if (nDst > nSrc)
                pDst[-1] = pSrc[0] >> nRShift;

            for (size_t i = 0; i < nSrc; i++)
            {
                pDst[i] = pSrc[i] << nBits;
                if (i + 1 < nSrc)
                    pDst[i] |= pSrc[i + 1] >> nRShift;
            }
        }
    }
    else
        memmove(pDst, pSrc, nSrc);

    memset0(pDst0 + nDst, nBytes);
}

void uintBigImpl::_Div(uint8_t* pDst, uint32_t nDst, const uint8_t* pA, uint32_t nA,
    const uint8_t* pB, uint32_t nB, uint8_t* pMul, uint8_t* pTmp)
{
    memset0(pDst, nDst);
    memset0(pMul, nA);

    uint32_t nOrder = _GetOrder(pB, nB);
    if (nOrder > (nA << 3))
        return;

    for (uint32_t nShift = 1 + std::min((nA << 3) - nOrder, (nDst << 3) - 1); nShift--;)
    {
        _ShiftLeft(pTmp, nA, pB, nB, nShift);
        _Inc(pTmp, nA, pMul, nA);

        if (_Cmp(pMul, nA, pTmp, nA) > 0)
            continue;  // overflow

        if (_Cmp(pA, nA, pTmp, nA) < 0)
            continue;  // exceeded

        memcpy(pMul, pTmp, nA);
        pDst[nDst - (nShift >> 3) - 1] |= (1 << (7 & nShift));
    }

#ifndef NDEBUG
    _Mul(pTmp, nA, pB, nB, pDst, nDst);
    assert(!_Cmp(pTmp, nA, pMul, nA));
#endif  // NDEBUG
}



/* ################## OS-specific functions ################## */

/*
 * returns physically available memory (no swap)
 */
static size_t getTotalPhysAvailableMemory()
{
#if defined(__APPLE__) || defined(__MACOSX)
    vm_statistics64_data_t	vm_stat;
    vm_size_t page_size;
    host_name_port_t host = mach_host_self();
    kern_return_t rv = host_page_size(host, &page_size);
    if( rv != KERN_SUCCESS) {
        cwarn << "Error in func " << __FUNCTION__ << " at host_page_size(...) \""
              << "\"\n";
        mach_error("host_page_size(...) error :", rv);
        return 0;
    }
    mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
    rv = host_statistics (host, HOST_VM_INFO, (host_info_t)&vm_stat, &count);
    if (rv != KERN_SUCCESS) {
        cwarn << "Error in func " << __FUNCTION__ << " at host_statistics(...) \""
              << "\"\n";
        mach_error("host_statistics(...) error :", rv);
        return 0;
    }
    return vm_stat.free_count*page_size;
#elif defined(__linux__)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    if (pages == -1L)
    {
        cwarn << "Error in func " << __FUNCTION__ << " at sysconf(_SC_AVPHYS_PAGES) \""
              << strerror(errno) << "\"\n";
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size == -1L)
    {
        cwarn << "Error in func " << __FUNCTION__ << " at sysconf(_SC_PAGESIZE) \""
              << strerror(errno) << "\"\n";
        return 0;
    }

    return (size_t)pages * (size_t)page_size;
#else
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo) == 0)
    {
        // Handle Errorcode (GetLastError) ??
        return 0;
    }
    return memInfo.ullAvailPhys;
#endif
}

/*
 * return numbers of available CPUs
 */
unsigned CPUMiner::getNumDevices()
{
#if 0
    static unsigned cpus = 0;

    if (cpus == 0)
    {
        std::vector< boost::fibers::numa::node > topo = boost::fibers::numa::topology();
        for (auto n : topo) {
            cpus += n.logical_cpus.size();
        }
    }
    return cpus;
#elif defined(__APPLE__) || defined(__MACOSX)
    unsigned int cpus_available = std::thread::hardware_concurrency();
    if (cpus_available <= 0)
    {
        cwarn << "Error in func " << __FUNCTION__ << " at std::thread::hardware_concurrency \""
              << cpus_available << " were found." << "\"\n";
        return 0;
    }
    return cpus_available;

#elif defined(__linux__)
    long cpus_available;
    cpus_available = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus_available == -1L)
    {
        cwarn << "Error in func " << __FUNCTION__ << " at sysconf(_SC_NPROCESSORS_ONLN) \""
              << strerror(errno) << "\"\n";
        return 0;
    }
    return cpus_available;
#else
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#endif
}


/* ######################## CPU Miner ######################## */

struct CPUChannel : public LogChannel
{
    static const char* name() { return EthOrange "cp"; }
    static const int verbosity = 2;
};
#define cpulog clog(CPUChannel)


CPUMiner::CPUMiner(unsigned _index, CPSettings _settings, DeviceDescriptor& _device)
  : Miner("cpu-", _index), m_settings(_settings)
{
    m_deviceDescriptor = _device;
}


CPUMiner::~CPUMiner()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cpulog, "cp-" << m_index << " CPUMiner::~CPUMiner() begin");
    stopWorking();
    kick_miner();
    DEV_BUILD_LOG_PROGRAMFLOW(cpulog, "cp-" << m_index << " CPUMiner::~CPUMiner() end");
}


/*
 * Bind the current thread to a spcific CPU
 */
bool CPUMiner::initDevice()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cpulog, "cp-" << m_index << " CPUMiner::initDevice begin");

    cpulog << "Using CPU: " << m_deviceDescriptor.cpCpuNumer << " " << m_deviceDescriptor.cuName
           << " Memory : " << dev::getFormattedMemory((double)m_deviceDescriptor.totalMemory);

#if defined(__APPLE__) || defined(__MACOSX)
/* Not supported on MAC OSX. See https://developer.apple.com/library/archive/releasenotes/Performance/RN-AffinityAPI/ */
#elif defined(__linux__)
    cpu_set_t cpuset;
    int err;

    CPU_ZERO(&cpuset);
    CPU_SET(m_deviceDescriptor.cpCpuNumer, &cpuset);

    err = sched_setaffinity(0, sizeof(cpuset), &cpuset);
    if (err != 0)
    {
        cwarn << "Error in func " << __FUNCTION__ << " at sched_setaffinity() \"" << strerror(errno)
              << "\"\n";
        cwarn << "cp-" << m_index << "could not bind thread to cpu" << m_deviceDescriptor.cpCpuNumer
              << "\n";
    }
#else
    DWORD_PTR dwThreadAffinityMask = 1i64 << m_deviceDescriptor.cpCpuNumer;
    DWORD_PTR previous_mask;
    previous_mask = SetThreadAffinityMask(GetCurrentThread(), dwThreadAffinityMask);
    if (previous_mask == NULL)
    {
        cwarn << "cp-" << m_index << "could not bind thread to cpu" << m_deviceDescriptor.cpCpuNumer
              << "\n";
        // Handle Errorcode (GetLastError) ??
    }
#endif
    DEV_BUILD_LOG_PROGRAMFLOW(cpulog, "cp-" << m_index << " CPUMiner::initDevice end");
    return true;
}


/*
 * A new epoch was receifed with last work package (called from Miner::initEpoch())
 *
 * If we get here it means epoch has changed so it's not necessary
 * to check again dag sizes. They're changed for sure
 * We've all related infos in m_epochContext (.dagSize, .dagNumItems, .lightSize, .lightNumItems)
 */
bool CPUMiner::initEpoch_internal()
{
    return true;
}


/*
   Miner should stop working on the current block
   This happens if a
     * new work arrived                       or
     * miner should stop (eg exit ethminer)   or
     * miner should pause
*/
void CPUMiner::kick_miner()
{
    m_new_work.store(true, std::memory_order_relaxed);
    m_new_work_signal.notify_one();
}


void CPUMiner::search(const dev::eth::WorkPackage& w)
{
    constexpr size_t blocksize = 30;

    //const auto& context = ethash::get_global_epoch_context_full(w.epoch);
    //const auto header = ethash::hash256_from_bytes(w.header.data());
    //const auto boundary = ethash::hash256_from_bytes(w.boundary.data());
    auto nonce = w.startNonce;

    const auto header = w.header.data();
    const auto boundary = w.boundary.data();
    auto target = uintBig_t<32>(boundary);

    while (true)
    {
        if (m_new_work.load(std::memory_order_relaxed))  // new work arrived ?
        {
            m_new_work.store(false, std::memory_order_relaxed);
            break;
        }

        if (shouldStop())
            break;


        //auto r = ethash::search(context, header, boundary, nonce, blocksize);
        //if (r.solution_found)
        //{
        //    h256 mix{reinterpret_cast<byte*>(r.mix_hash.bytes), h256::ConstructFromPointer};
        //    auto sol = Solution{r.nonce, mix, w, std::chrono::steady_clock::now(), m_index};

        //    cpulog << EthWhite << "Job: " << w.header.abridged()
        //           << " Sol: " << toHex(sol.nonce, HexPrefix::Add) << EthReset;
        //    Farm::f().submitProof(sol);
        //}

        auto nonceHex = toHex(nonce);
        auto nonceBytes = fromHex(nonceHex);
        byte pDataIn[40];
        byte pDataOut[32];
        memcpy(pDataIn, (unsigned char*)header, 32);
        memcpy(pDataIn, nonceBytes.data(), 8);

        if (w.algoType == 0){

        }
        else{

        }

        const h256 out{reinterpret_cast<byte*>(pDataOut), h256::ConstructFromPointer};
        const auto hash = uintBig_t<32>(out.data());
        if (hash.cmp(target) <= 0){
            {
                auto sol = Solution{nonce, h256{}, w, std::chrono::steady_clock::now(), m_index};

                cpulog << EthWhite << "Job: " << w.header.abridged()
                       << " Sol: " << toHex(sol.nonce, HexPrefix::Add) << EthReset;
                Farm::f().submitProof(sol);
            }
        }

        nonce += blocksize;

        // Update the hash rate
        updateHashRate(blocksize, 1);
    }
}


/*
 * The main work loop of a Worker thread
 */
void CPUMiner::workLoop()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cpulog, "cp-" << m_index << " CPUMiner::workLoop() begin");

    WorkPackage current;
    current.header = h256();

    if (!initDevice())
        return;

    while (!shouldStop())
    {
        // Wait for work or 3 seconds (whichever the first)
        const WorkPackage w = work();
        if (!w)
        {
            boost::system_time const timeout =
                boost::get_system_time() + boost::posix_time::seconds(3);
            boost::mutex::scoped_lock l(x_work);
            m_new_work_signal.timed_wait(l, timeout);
            continue;
        }

        if (w.algo == "ethash")
        {
            // Epoch change ?
            if (current.epoch != w.epoch)
            {
                if (!initEpoch())
                    break;  // This will simply exit the thread

                // As DAG generation takes a while we need to
                // ensure we're on latest job, not on the one
                // which triggered the epoch change
                current = w;
                continue;
            }

            // Persist most recent job.
            // Job's differences should be handled at higher level
            current = w;

            // Start searching
            search(w);
        }
        else
        {
            throw std::runtime_error("Algo : " + w.algo + " not yet implemented");
        }
    }

    DEV_BUILD_LOG_PROGRAMFLOW(cpulog, "cp-" << m_index << " CPUMiner::workLoop() end");
}


void CPUMiner::enumDevices(std::map<string, DeviceDescriptor>& _DevicesCollection)
{
    unsigned numDevices = getNumDevices();

    for (unsigned i = 0; i < numDevices; i++)
    {
        string uniqueId;
        ostringstream s;
        DeviceDescriptor deviceDescriptor;

        s << "cpu-" << i;
        uniqueId = s.str();
        if (_DevicesCollection.find(uniqueId) != _DevicesCollection.end())
            deviceDescriptor = _DevicesCollection[uniqueId];
        else
            deviceDescriptor = DeviceDescriptor();

        s.str("");
        s.clear();
        s << "ethash::eval()/boost " << (BOOST_VERSION / 100000) << "."
          << (BOOST_VERSION / 100 % 1000) << "." << (BOOST_VERSION % 100);
        deviceDescriptor.name = s.str();
        deviceDescriptor.uniqueId = uniqueId;
        deviceDescriptor.type = DeviceTypeEnum::Cpu;
        deviceDescriptor.totalMemory = getTotalPhysAvailableMemory();

        deviceDescriptor.cpCpuNumer = i;

        _DevicesCollection[uniqueId] = deviceDescriptor;
    }
}
