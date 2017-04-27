// Copyright 2009-2017 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2017, Sandia Corporation
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef SST_SCHEDULER_UTIL_H__
#define SST_SCHEDULER_UTIL_H__

#include <string>
#include <sys/types.h>

namespace SST {
namespace Scheduler {
namespace Utils {

bool file_exists(const std::string &path);
time_t file_time_last_written(const std::string &path);

}
}
}
#endif