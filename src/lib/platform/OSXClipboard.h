/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClipboard.h"

#include <Carbon/Carbon.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class IOSXClipboardConverter;

//! OS X clipboard implementation
class OSXClipboard : public IClipboard
{
public:
  OSXClipboard();
  virtual ~OSXClipboard();

  //! Test if clipboard is owned by deskflow
  static bool isOwnedByDeskflow();

  // IClipboard overrides
  bool empty() override;
  void add(Format, const std::string &data) override;
  bool open(Time) const override;
  void close() const override;
  Time getTime() const override;
  bool has(Format) const override;
  std::string get(Format) const override;

  bool synchronize();

  //! Test if the clipboard came from another device via Universal Clipboard
  bool isRemote() const;

  //! Test if a read that timed out has since completed
  /*!
  Returns true, once, when the data for a read that get() gave up on has
  arrived, so the clipboard is worth reading again.
  */
  bool takeLateFetch();

private:
  void clearConverters();
  bool fetch(PasteboardItemID item, CFStringRef type, std::string &data) const;

private:
  using ConverterList = std::vector<IOSXClipboardConverter *>;

  mutable Time m_time;
  ConverterList m_converters;
  PasteboardRef m_pboard;

  // bumped every time the pasteboard contents change
  mutable uint64_t m_generation = 1;
  // generation whose read timed out, 0 if none; reads are skipped while it's current
  mutable uint64_t m_abandonedGeneration = 0;
  // true once the current generation has been re-read after a late fetch
  mutable bool m_retried = false;
  // generation of a timed out read that has since completed, 0 if none.
  // shared with the fetch threads, which can outlive this object.
  std::shared_ptr<std::atomic<uint64_t>> m_lateGeneration = std::make_shared<std::atomic<uint64_t>>(0);
};

//! Clipboard format converter interface
/*!
This interface defines the methods common to all Scrap book format
*/
class IOSXClipboardConverter
{
public:
  virtual ~IOSXClipboardConverter() = default;
  //! @name accessors
  //@{

  //! Get clipboard format
  /*!
  Return the clipboard format this object converts from/to.
  */
  virtual IClipboard::Format getFormat() const = 0;

  //! returns the scrap flavor type that this object converts from/to
  virtual CFStringRef getOSXFormat() const = 0;

  //! Convert from IClipboard format
  /*!
  Convert from the IClipboard format to the Carbon scrap format.
  The input data must be in the IClipboard format returned by
  getFormat().  The return data will be in the scrap
  format returned by getOSXFormat().
  */
  virtual std::string fromIClipboard(const std::string &) const = 0;

  //! Convert to IClipboard format
  /*!
  Convert from the carbon scrap format to the IClipboard format
  (i.e., the reverse of fromIClipboard()).
  */
  virtual std::string toIClipboard(const std::string &) const = 0;

  //@}
};
