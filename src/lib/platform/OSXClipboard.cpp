/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboard.h"

#include "arch/ArchException.h"
#include "base/Log.h"
#include "platform/OSXClipboardBMPConverter.h"
#include "platform/OSXClipboardHTMLConverter.h"
#include "platform/OSXClipboardTextConverter.h"
#include "platform/OSXClipboardUTF16Converter.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

// how long a read may hold up the event thread. reading a flavor whose data
// isn't on the pasteboard yet blocks until the sender provides it, and for a
// Universal Clipboard item the sender is another Mac, which can take longer
// than the keep alive timeout. this must stay under kKeepAliveRate.
const auto kFetchTimeout = std::chrono::seconds(2);

// flavor present on items that came from another device via Universal Clipboard
const CFStringRef kRemoteClipboardFlavor = CFSTR("com.apple.is-remote-clipboard");

struct FetchState
{
  std::mutex m_mutex;
  std::condition_variable m_cond;
  bool m_done = false;
  bool m_abandoned = false;
  OSStatus m_err = noErr;
  std::string m_data;
};

} // namespace

//
// OSXClipboard
//

OSXClipboard::OSXClipboard() : m_time(0), m_pboard(nullptr)
{
  m_converters.push_back(new OSXClipboardHTMLConverter);
  m_converters.push_back(new OSXClipboardBMPConverter);
  m_converters.push_back(new OSXClipboardUTF8Converter);
  m_converters.push_back(new OSXClipboardUTF16Converter);
  m_converters.push_back(new OSXClipboardTextConverter);

  OSStatus createErr = PasteboardCreate(kPasteboardClipboard, &m_pboard);
  if (createErr != noErr) {
    LOG_WARN("failed to create clipboard reference: error %i", createErr);
    LOG_ERR("unable to connect to pasteboard, clipboard sharing disabled", createErr);
    m_pboard = nullptr;
    return;
  }

  OSStatus syncErr = PasteboardSynchronize(m_pboard);
  if (syncErr != noErr) {
    LOG_WARN("failed to syncronize clipboard: error %i", syncErr);
  }
}

OSXClipboard::~OSXClipboard()
{
  clearConverters();
}

bool OSXClipboard::empty()
{
  LOG_DEBUG("emptying clipboard");
  if (m_pboard == nullptr)
    return false;

  OSStatus err = PasteboardClear(m_pboard);
  if (err != noErr) {
    LOG_WARN("failed to clear clipboard: error %i", err);
    return false;
  }

  return true;
}

bool OSXClipboard::synchronize()
{
  if (m_pboard == nullptr)
    return false;

  PasteboardSyncFlags flags = PasteboardSynchronize(m_pboard);
  LOG_VERBOSE("flags: %x", flags);

  if (flags & kPasteboardModified) {
    ++m_generation;
    m_retried = false;
    return true;
  }
  return false;
}

bool OSXClipboard::isRemote() const
{
  if (m_pboard == nullptr)
    return false;

  PasteboardItemID item;
  PasteboardFlavorFlags flags;
  return PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item) == noErr &&
         PasteboardGetItemFlavorFlags(m_pboard, item, kRemoteClipboardFlavor, &flags) == noErr;
}

bool OSXClipboard::takeLateFetch()
{
  const uint64_t generation = m_lateGeneration->exchange(0);
  if (generation == 0 || generation != m_generation || generation != m_abandonedGeneration || m_retried) {
    return false;
  }

  LOG_DEBUG("clipboard data arrived after read timed out");
  m_abandonedGeneration = 0;
  m_retried = true;
  return true;
}

void OSXClipboard::add(Format format, const std::string &data)
{
  if (m_pboard == nullptr)
    return;

  LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
  if (format == IClipboard::Format::Text) {
    LOG_DEBUG("format of data to be added to clipboard was kText");
  } else if (format == IClipboard::Format::Bitmap) {
    LOG_DEBUG("format of data to be added to clipboard was kBitmap");
  } else if (format == IClipboard::Format::HTML) {
    LOG_DEBUG("format of data to be added to clipboard was kHTML");
  }

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    IOSXClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      std::string osXData = converter->fromIClipboard(data);
      CFStringRef flavorType = converter->getOSXFormat();
      CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (uint8_t *)osXData.data(), osXData.size());
      PasteboardItemID itemID = 0;

      if (dataRef) {
        PasteboardPutItemFlavor(m_pboard, itemID, flavorType, dataRef, kPasteboardFlavorNoFlags);

        CFRelease(dataRef);
        LOG_DEBUG("added %d bytes to clipboard format: %d", data.size(), format);
      }
    }
  }
}

bool OSXClipboard::open(Time time) const
{
  if (m_pboard == nullptr)
    return false;

  LOG_DEBUG("opening clipboard");
  m_time = time;
  return true;
}

void OSXClipboard::close() const
{
  LOG_DEBUG("closing clipboard");
  /* not needed */
}

IClipboard::Time OSXClipboard::getTime() const
{
  return m_time;
}

bool OSXClipboard::has(Format format) const
{
  if (m_pboard == nullptr)
    return false;

  // a read of this clipboard timed out, don't try the other formats
  if (m_abandonedGeneration == m_generation)
    return false;

  PasteboardItemID item;
  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IOSXClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      PasteboardFlavorFlags flags;
      CFStringRef type = converter->getOSXFormat();

      OSStatus res;

      if ((res = PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags)) == noErr) {
        return true;
      }
    }
  }

  return false;
}

std::string OSXClipboard::get(Format format) const
{
  CFStringRef type;
  PasteboardItemID item;
  std::string result;

  if (m_pboard == nullptr)
    return result;

  if (m_abandonedGeneration == m_generation)
    return result;

  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  // find the converter for the first clipboard format we can handle
  IOSXClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    converter = *index;

    PasteboardFlavorFlags flags;
    type = converter->getOSXFormat();

    if (converter->getFormat() == format && PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags) == noErr) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_DEBUG("unable to find converter for data");
    return result;
  }

  // get the clipboard data.
  if (!fetch(item, type, result)) {
    return std::string();
  }

  return converter->toIClipboard(result);
}

bool OSXClipboard::fetch(PasteboardItemID item, CFStringRef type, std::string &data) const
{
  // read on another thread so that a slow sender can't stall the event
  // thread. the thread uses its own pasteboard reference; the pasteboard
  // functions aren't safe to call on one reference from two threads.
  auto state = std::make_shared<FetchState>();
  try {
    std::thread([state, type, late = m_lateGeneration, generation = m_generation] {
      OSStatus err;
      std::string buffer;
      PasteboardRef pboard = nullptr;
      if ((err = PasteboardCreate(kPasteboardClipboard, &pboard)) == noErr) {
        PasteboardSynchronize(pboard);

        PasteboardItemID threadItem;
        CFDataRef dataRef = nullptr;
        if ((err = PasteboardGetItemIdentifier(pboard, (CFIndex)1, &threadItem)) == noErr &&
            (err = PasteboardCopyItemFlavorData(pboard, threadItem, type, &dataRef)) == noErr && dataRef != nullptr) {
          buffer.assign((const char *)CFDataGetBytePtr(dataRef), CFDataGetLength(dataRef));
          CFRelease(dataRef);
        }
        CFRelease(pboard);
      }

      std::scoped_lock lock(state->m_mutex);
      state->m_err = err;
      state->m_data = std::move(buffer);
      state->m_done = true;
      if (state->m_abandoned) {
        late->store(generation);
      }
      state->m_cond.notify_one();
    }).detach();
  } catch (const std::system_error &e) {
    LOG_WARN("failed to start clipboard read: %s", e.what());
    return false;
  }

  std::unique_lock lock(state->m_mutex);
  if (!state->m_cond.wait_for(lock, kFetchTimeout, [&state] { return state->m_done; })) {
    state->m_abandoned = true;
    lock.unlock();
    m_abandonedGeneration = m_generation;

    PasteboardFlavorFlags flags = 0;
    PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags);
    const bool remote = isRemote();
    LOG_WARN(
        "clipboard read timed out after %d seconds%s%s, %s", static_cast<int>(kFetchTimeout.count()),
        remote ? " (universal clipboard item)" : "", (flags & kPasteboardFlavorPromised) ? " (promised data)" : "",
        m_retried ? "giving up on this clipboard" : "will retry when the data arrives"
    );
    return false;
  }

  if (state->m_err != noErr) {
    LOG_DEBUG("failed to read clipboard data: error %d", state->m_err);
    return false;
  }

  data = std::move(state->m_data);
  return true;
}

void OSXClipboard::clearConverters()
{
  if (m_pboard == nullptr)
    return;

  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}
