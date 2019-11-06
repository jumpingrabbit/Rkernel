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


#include "RLazyGraphicsDevice.h"

#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>

#include "../Evaluator.h"
#include "REagerGraphicsDevice.h"
#include "actions/RCircleAction.h"
#include "actions/RClipAction.h"
#include "actions/RLineAction.h"
#include "actions/RModeAction.h"
#include "actions/RNewPageAction.h"
#include "actions/RPolygonAction.h"
#include "actions/RPolylineAction.h"
#include "actions/RRectAction.h"
#include "actions/RPathAction.h"
#include "actions/RRasterAction.h"
#include "actions/RTextAsciiAction.h"
#include "actions/RTextUtf8Action.h"
#include "actions/util/RescaleUtil.h"

namespace graphics {
namespace devices {
namespace {

const auto GAP_SEQUENCE = "m";

const auto NORMAL_SUFFIX = "normal";
const auto SKETCH_SUFFIX = "sketch";
const auto ZOOMED_SUFFIX = "zoomed";
const auto EXPORT_SUFFIX = "export";
const auto MARGIN_SUFFIX = "margin";

const char* getSuffixBySnapshotType(SnapshotType type) {
  switch (type) {
    case SnapshotType::NORMAL:
      return NORMAL_SUFFIX;
    case SnapshotType::ZOOMED:
      return ZOOMED_SUFFIX;
    case SnapshotType::EXPORT:
      return EXPORT_SUFFIX;
    default:
      throw std::runtime_error("Unsupported snapshot type #" + std::to_string(int(type)));
  }
}

}  // anonymous

RLazyGraphicsDevice::RLazyGraphicsDevice(std::string snapshotDirectory, int snapshotNumber, ScreenParameters parameters)
    : snapshotDirectory(std::move(snapshotDirectory)), snapshotNumber(snapshotNumber), snapshotVersion(0),
      parameters(parameters), slave(nullptr),
      artBoard(buildCurrentCanvas()), areMarginsLarge(false), hasDrawnLine(false)
{
  labelGroups = LabelGroups{LabelGroup(), LabelGroup(), LabelGroup(), LabelGroup()};  // NORTH, WEST, SOUTH, EAST
  DEVICE_TRACE;
}

RLazyGraphicsDevice::RLazyGraphicsDevice(ActionList previousActions, LabelGroups labelGroups, Rectangle artBoard,
                                         std::string snapshotDirectory, int snapshotNumber,
                                         ScreenParameters parameters)
    : RLazyGraphicsDevice(std::move(snapshotDirectory), snapshotNumber, parameters)
{
  DEVICE_TRACE;
  this->previousActions = std::move(previousActions);
  this->labelGroups = std::move(labelGroups);
  this->artBoard = artBoard;
}

RLazyGraphicsDevice::ActionList RLazyGraphicsDevice::copyActions() {
  auto copy = ActionList();
  for (auto &action : previousActions) {
    copy.push_back(action->clone());
  }
  for (auto &action : actions) {
    copy.push_back(action->clone());
  }
  return copy;
}

RLazyGraphicsDevice::LabelGroups RLazyGraphicsDevice::copyLabels() {
  auto copy = labelGroups;
  for (auto &labelGroup : copy) {
    for (auto &label : labelGroup.labels) {
      if (!label.isFromPreviousActions) {
        auto numPrevious = int(previousActions.size());
        label.actionIndex += numPrevious;
        label.isFromPreviousActions = true;
      }
    }
  }
  return copy;
}

void RLazyGraphicsDevice::applyActions(const RLazyGraphicsDevice::ActionList &actionList) {
  for (auto &action : actionList) {
    action->perform(slave);
  }
}

Ptr<RGraphicsDevice> RLazyGraphicsDevice::initializeSlaveDevice(const std::string &path) {
  DEVICE_TRACE;
  return makePtr<REagerGraphicsDevice>(path, parameters);
}

void RLazyGraphicsDevice::shutdownSlaveDevice() {
  slave = nullptr;  // Note: this will trigger slave's dtor as well
}

Rectangle RLazyGraphicsDevice::buildCanvas(double width, double height) {
  return Rectangle{
      Point{0.0, 0.0},
      Point{width, height}
  };
}

Rectangle RLazyGraphicsDevice::buildCurrentCanvas() {
  auto size = screenParameters().size;
  return buildCanvas(size.width, size.height);
}

std::string RLazyGraphicsDevice::buildSnapshotPath(const char* typeSuffix, const char* errorSuffix) {
  DEVICE_TRACE;
  auto sout = std::ostringstream();
  sout << snapshotDirectory << "/snapshot_" << typeSuffix;
  if (errorSuffix) {
    sout << "_" << errorSuffix;
  }
  sout << "_" << snapshotNumber << "_" << snapshotVersion << ".png";
  return sout.str();
}

void RLazyGraphicsDevice::adjustLabels() {
  for (auto &labelGroup : labelGroups) {
    auto &labels = labelGroup.labels;
    if (labels.size() >= 2U) {
      auto numLabels = int(labels.size());
      auto textActions = TextActionList();
      for (auto &label : labels) {
        textActions.push_back(getTextActionForLabel(label));
      }
      auto lastEnabledIndex = 0;
      for (auto i = 1; i < numLabels; i++) {  // Note: the first element was intentionally skipped
        auto textAction = textActions[i];
        auto lastEnabledAction = textActions[lastEnabledIndex];
        auto actualDistance = distance(textAction->location(), lastEnabledAction->location());
        auto minDistance = (textAction->textWidth() + lastEnabledAction->textWidth()) / 2.0 + labelGroup.gap;
        if (actualDistance > minDistance) {
          textAction->setEnabled(true);
          lastEnabledIndex = i;
        } else {
          textAction->setEnabled(false);
        }
      }
    }
  }
}

Ptr<actions::RTextAction> RLazyGraphicsDevice::getTextActionForLabel(RLazyGraphicsDevice::LabelInfo label) {
  auto &actionList = label.isFromPreviousActions ? previousActions : actions;
  auto action = actionList[label.actionIndex];
  auto textAction = std::dynamic_pointer_cast<actions::RTextAction>(action);
  if (!textAction) {
    std::cerr << "Failed to downcast to RTextAction: actionIndex = " << label.actionIndex
              << ", is previous = " << label.isFromPreviousActions << std::endl;
    throw std::runtime_error("Downcast to RTextAction failed");
  }
  return textAction;
}

void RLazyGraphicsDevice::drawCircle(Point center, double radius, pGEcontext context) {
  DEVICE_TRACE;
  actions.push_back(makePtr<actions::RCircleAction>(center, radius, context));
}

void RLazyGraphicsDevice::clip(Point from, Point to) {
  DEVICE_TRACE;
  auto candidateArtBoard = Rectangle::make(from, to);
  if (!isClose(buildCurrentCanvas(), candidateArtBoard)) {
    artBoard = candidateArtBoard;
  }
  actions.push_back(makePtr<actions::RClipAction>(from, to));
}

void RLazyGraphicsDevice::close() {
  DEVICE_TRACE;
  // Do nothing for now
}

void RLazyGraphicsDevice::drawLine(Point from, Point to, pGEcontext context) {
  DEVICE_TRACE;
  hasDrawnLine = true;
  actions.push_back(makePtr<actions::RLineAction>(from, to, context));
}

MetricInfo RLazyGraphicsDevice::metricInfo(int character, pGEcontext context) {
  DEVICE_TRACE;
  return getSlave()->metricInfo(character, context);
}

void RLazyGraphicsDevice::setMode(int mode) {
  DEVICE_TRACE;
  if (mode == 0) {
    hasDrawnLine = false;
  }
  actions.push_back(makePtr<actions::RModeAction>(mode));
}

void RLazyGraphicsDevice::newPage(pGEcontext context) {
  DEVICE_TRACE;
  previousActions.clear();
  for (auto &group : labelGroups) {
    group.labels.clear();
  }
  hasDrawnLine = false;
  actions.push_back(makePtr<actions::RNewPageAction>(context));
}

void RLazyGraphicsDevice::drawPolygon(const std::vector<Point> &points, pGEcontext context) {
  DEVICE_TRACE;
  actions.push_back(makePtr<actions::RPolygonAction>(points, context));
}

void RLazyGraphicsDevice::drawPolyline(const std::vector<Point> &points, pGEcontext context) {
  DEVICE_TRACE;
  actions.push_back(makePtr<actions::RPolylineAction>(points, context));
}

void RLazyGraphicsDevice::drawRect(Point from, Point to, pGEcontext context) {
  DEVICE_TRACE;
  actions.push_back(makePtr<actions::RRectAction>(from, to, context));
}

void RLazyGraphicsDevice::drawPath(const std::vector<Point> &points, const std::vector<int> &numPointsPerPolygon,
                                   Rboolean winding, pGEcontext context)
{
  DEVICE_TRACE;
  actions.push_back(makePtr<actions::RPathAction>(points, numPointsPerPolygon, winding, context));
}

void RLazyGraphicsDevice::drawRaster(const RasterInfo &rasterInfo, Point at, Size size, double rotation,
                                     Rboolean interpolate, pGEcontext context)
{
  DEVICE_TRACE;
  actions.push_back(makePtr<actions::RRasterAction>(rasterInfo, at, size, artBoard, rotation, interpolate, context));
}

ScreenParameters RLazyGraphicsDevice::screenParameters() {
  DEVICE_TRACE;
  return getSlave()->screenParameters();
}

double RLazyGraphicsDevice::widthOfStringUtf8(const char* text, pGEcontext context) {
  DEVICE_TRACE;
  return getSlave()->widthOfStringUtf8(text, context);
}

void RLazyGraphicsDevice::drawTextUtf8(const char* text, Point at, double rotation, double heightAdjustment, pGEcontext context) {
  DEVICE_TRACE;
  if (hasDrawnLine) {
    auto relative = actions::util::getRelativePosition(at, artBoard);
    switch (relative) {
      case actions::util::RelativePosition::NORTH:
      case actions::util::RelativePosition::WEST:
      case actions::util::RelativePosition::SOUTH:
      case actions::util::RelativePosition::EAST: {
        auto actionIndex = int(actions.size());
        auto listIndex = int(relative);  // Note: take advantage of enum values order
        auto &labelGroup = labelGroups[listIndex];
        if (labelGroup.labels.empty()) {
          labelGroup.gap = widthOfStringUtf8(GAP_SEQUENCE, context);
        }
        labelGroup.labels.push_back(LabelInfo{actionIndex, false});
        break;
      }
      default: {
        break;
      }
    }
  }
  // Note: it looks like engine calls `widthOfStringUtf8` just before `drawTextUtf8`
  // so there might be a temptation to cache it
  // but experiments have shown that this assumption is not always true
  auto textWidth = widthOfStringUtf8(text, context);
  actions.push_back(makePtr<actions::RTextUtf8Action>(text, textWidth, at, rotation, heightAdjustment, context));
}

bool RLazyGraphicsDevice::dump(SnapshotType type) {
  DEVICE_TRACE;
  if (!areMarginsLarge) {
    shutdownSlaveDevice();
    if (!actions.empty()) {
      getSlave(getSuffixBySnapshotType(type));
      applyActions(previousActions);
      applyActions(actions);
      shutdownSlaveDevice();
      snapshotVersion++;
      return true;
    } else {
      return false;
    }
  } else {
    auto path = buildSnapshotPath(getSuffixBySnapshotType(type), MARGIN_SUFFIX);
    auto fout = std::ofstream(path);
    fout.close();
    snapshotVersion++;
    return true;
  }
}

Ptr<RGraphicsDevice> RLazyGraphicsDevice::getSlave(const char* typeSuffix) {
  if (!slave) {
    slave = initializeSlaveDevice(buildSnapshotPath(typeSuffix != nullptr ? typeSuffix : SKETCH_SUFFIX));
  }
  return slave;
}

void RLazyGraphicsDevice::rescale(double newWidth, double newHeight) {
  DEVICE_TRACE;
  if (!isClose(newWidth, parameters.size.width) || !isClose(newHeight, parameters.size.height)) {
    // Note: on Mac actual canvas size is not equal to `parameters.size`.
    auto oldCanvas = buildCurrentCanvas();

    // Note: `scaleFactor` is exactly `(75.0 / parameters.resolution) * 0.96` for Mac.
    // But why hard-code this odd formula when we can just use provided canvas size?
    auto scaleFactor = oldCanvas.width() / parameters.size.width;
    auto newCanvas = buildCanvas(newWidth * scaleFactor, newHeight * scaleFactor);
    auto deltaFrom = artBoard.from - oldCanvas.from;
    auto deltaTo = artBoard.to - oldCanvas.to;
    auto newArtBoard = Rectangle{
        newCanvas.from + deltaFrom,
        newCanvas.to + deltaTo
    };
    if (newArtBoard.height() > 0 && newArtBoard.width() > 0) {
      areMarginsLarge = false;
      auto xScale = newArtBoard.width() / artBoard.width();
      auto yScale = newArtBoard.height() / artBoard.height();
      auto rescaleInfo = actions::RescaleInfo{
          artBoard, newArtBoard, Point{xScale, yScale}
      };
      for (auto &action : previousActions) {
        action->rescale(rescaleInfo);
      }
      for (auto &action : actions) {
        action->rescale(rescaleInfo);
      }
      adjustLabels();
      parameters.size.width = newWidth;
      parameters.size.height = newHeight;
      artBoard = newArtBoard;
      shutdownSlaveDevice();
    } else {
      areMarginsLarge = true;
    }
  }
}

Ptr<RGraphicsDevice> RLazyGraphicsDevice::clone() {
  // Note: I don't use `makePtr` here since ctor is private
  return ptrOf(new RLazyGraphicsDevice(copyActions(), copyLabels(), artBoard, snapshotDirectory, snapshotNumber + 1, parameters));
}

bool RLazyGraphicsDevice::isBlank() {
  for (auto &action : actions) {
    if (action->isVisible()) {
      return false;
    }
  }
  return true;
}

}  // devices
}  // graphics
