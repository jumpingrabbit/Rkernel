//  Rkernel is an execution kernel for R interpreter
//  Copyright (C) 2019 JetBrains s.r.o.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.


#ifndef MASTER_DEVICE_H
#define MASTER_DEVICE_H

#include <string>

#include "ScreenParameters.h"

namespace graphics {

class MasterDevice {
public:
  static void init(const std::string& snapshotDirectory, ScreenParameters screenParameters);
  static void dumpAndMoveNext();
  static bool rescaleAllLast(double width, double height);
  static bool rescaleByNumber(int number, double width, double height);
};

}  // graphics

#endif // MASTER_DEVICE_H
