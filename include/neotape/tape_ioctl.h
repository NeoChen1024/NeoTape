#pragma once

#include <sys/ioctl.h>

namespace mt {

// -----------------------------------------------------------------------
// ioctl parameter / result structs  (kernel ABI — facts, not expression)
// -----------------------------------------------------------------------

struct mtop  { short mt_op; int mt_count; };
struct mtpos { long mt_blkno; };
struct mtget {
    long mt_type, mt_resid, mt_dsreg, mt_gstat, mt_erreg;
    int  mt_fileno, mt_blkno;
};

// -----------------------------------------------------------------------
// ioctl command numbers
// -----------------------------------------------------------------------

inline constexpr unsigned long MTIOCTOP = _IOW('m', 1, mtop);
inline constexpr unsigned long MTIOCGET = _IOR('m', 2, mtget);
inline constexpr unsigned long MTIOCPOS = _IOR('m', 3, mtpos);

// -----------------------------------------------------------------------
// MTIOCTOP operation codes
// -----------------------------------------------------------------------

enum : short {
    MTWEOF   = 0,   // write filemark
    MTFSF    = 1,   // forward space filemark
    MTBSF    = 2,   // backward space filemark
    MTFSR    = 3,   // forward space record
    MTBSR    = 4,   // backward space record
    MTREW    = 6,   // rewind
    MTOFFL   = 7,   // rewind + offline
    MTNOP    = 8,   // no-op
    MTRETEN  = 9,   // retension
    MTEOM    = 11,  // space to end of recorded media
    MTERASE  = 12,  // erase
    MTFSFM   = 13,  // forward space to filemark
    MTBSFM   = 14,  // backward space to filemark
    MTSEEK   = 15,  // seek to block
    MTTELL   = 16,  // tell block number
    MTFSS    = 17,  // forward space setmark
    MTBSS    = 18,  // backward space setmark
    MTSETBLK = 20,  // set block length
    MTSETDENSITY = 21, // set density
    MTWSM    = 22,  // write setmark
    MTLOCK   = 23,  // lock drive
    MTUNLOCK = 24,  // unlock drive
    MTLOAD   = 25,  // load media
    MTUNLOAD = 26,  // unload media
    MTCOMPRESSION = 27, // set compression
    MTSETPART = 28, // set partition
    MTMKPART = 29,  // make partition
    MTWEOFI  = 30,  // write EOT filemark
};

// -----------------------------------------------------------------------
// Status bits (mt_gstat masks)
// -----------------------------------------------------------------------

inline constexpr long GMT_EOF     = 0x80000000;
inline constexpr long GMT_BOT     = 0x40000000;
inline constexpr long GMT_EOT     = 0x20000000;
inline constexpr long GMT_SM      = 0x10000000;
inline constexpr long GMT_EOD     = 0x08000000;
inline constexpr long GMT_WR_PROT = 0x04000000;
inline constexpr long GMT_ONLINE  = 0x01000000;
inline constexpr long GMT_DR_OPEN = 0x00040000;
inline constexpr long GMT_CLN     = 0x00008000;

// -----------------------------------------------------------------------
// Density / block-size field layout in dsreg
// -----------------------------------------------------------------------

inline constexpr long MT_ST_DENSITY_MASK  = 0xff000000;
inline constexpr int  MT_ST_DENSITY_SHIFT = 24;
inline constexpr long MT_ST_BLKSIZE_MASK  = 0x00ffffff;
inline constexpr int  MT_ST_BLKSIZE_SHIFT = 0;

// -----------------------------------------------------------------------
// Drive type constants (mt_type)
// -----------------------------------------------------------------------

inline constexpr long MT_ISSCSI1       = 0x01;
inline constexpr long MT_ISSCSI2       = 0x02;
inline constexpr long MT_ISONSTREAM_SC = 0x04;

} // namespace mt
