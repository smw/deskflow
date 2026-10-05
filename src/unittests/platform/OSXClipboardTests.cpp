/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXClipboardTests.h"

#include "platform/OSXClipboard.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <QElapsedTimer>
#include <QScopeGuard>

#include <atomic>
#include <thread>

#include <unistd.h>

namespace {

// keeps a promise too slowly for OSXClipboard::get() to wait for it
OSStatus slowPromiseKeeper(PasteboardRef pboard, PasteboardItemID item, CFStringRef type, void *)
{
  sleep(4);
  const char text[] = "late";
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const uint8_t *)text, sizeof(text) - 1);
  OSStatus err = PasteboardPutItemFlavor(pboard, item, type, data, kPasteboardFlavorNoFlags);
  CFRelease(data);
  return err;
}

} // namespace

void OSXClipboardTests::open()
{
  OSXClipboard clipboard;
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
  clipboard.close();
}

void OSXClipboardTests::singleFormat()
{
  using enum IClipboard::Format;

  OSXClipboard clipboard;
  QVERIFY(clipboard.empty());
  clipboard.add(Text, m_testString);
  QVERIFY(clipboard.has(Text));
  QCOMPARE(clipboard.get(Text), m_testString);
}

void OSXClipboardTests::formatConvert_UTF8()
{
  OSXClipboardUTF8Converter converter;
  QCOMPARE(IClipboard::Format::Text, converter.getFormat());
  QCOMPARE(converter.getOSXFormat(), CFSTR("public.utf8-plain-text"));
  QCOMPARE(converter.fromIClipboard("test data\n"), "test data\r");
  QCOMPARE(converter.toIClipboard("test data\r"), "test data\n");
}

void OSXClipboardTests::remoteMarker()
{
  using enum IClipboard::Format;

  OSXClipboard clipboard;
  QVERIFY(clipboard.empty());
  clipboard.add(Text, m_testString);
  QVERIFY(!clipboard.isRemote());

  // replace it with an item marked the way universal clipboard does
  PasteboardRef pboard = nullptr;
  QCOMPARE(PasteboardCreate(kPasteboardClipboard, &pboard), noErr);
  QCOMPARE(PasteboardClear(pboard), noErr);
  PasteboardSynchronize(pboard);
  const uint8_t marker = 1;
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, &marker, sizeof(marker));
  for (CFStringRef type : {CFSTR("public.utf8-plain-text"), CFSTR("com.apple.is-remote-clipboard")}) {
    QCOMPARE(PasteboardPutItemFlavor(pboard, (PasteboardItemID)1, type, data, kPasteboardFlavorNoFlags), noErr);
  }
  CFRelease(data);
  CFRelease(pboard);

  clipboard.synchronize();
  QVERIFY(clipboard.isRemote());
}

void OSXClipboardTests::slowPromise()
{
  using enum IClipboard::Format;

  // put a promise on the clipboard from a thread that stays around to keep it
  std::atomic<bool> promised = false;
  std::atomic<bool> finished = false;
  std::thread owner([&promised, &finished] {
    PasteboardRef pboard = nullptr;
    if (PasteboardCreate(kPasteboardClipboard, &pboard) == noErr && PasteboardClear(pboard) == noErr &&
        PasteboardSetPromiseKeeper(pboard, &slowPromiseKeeper, nullptr) == noErr &&
        PasteboardPutItemFlavor(
            pboard, (PasteboardItemID)1, CFSTR("public.utf8-plain-text"), kPasteboardPromisedData,
            kPasteboardFlavorNoFlags
        ) == noErr) {
      promised = true;
      while (!finished) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
      }
    }
    finished = true;
    if (pboard != nullptr) {
      CFRelease(pboard);
    }
  });
  const auto joinOwner = qScopeGuard([&owner, &finished] {
    finished = true;
    owner.join();
  });
  QTRY_VERIFY(promised || finished);
  QVERIFY(promised);

  OSXClipboard clipboard;
  clipboard.synchronize();
  QVERIFY(clipboard.has(Text));

  // the read gives up rather than waiting for the promise to be kept
  QElapsedTimer timer;
  timer.start();
  QCOMPARE(clipboard.get(Text), std::string());
  QVERIFY(timer.elapsed() < 3000);

  // and the clipboard is left alone until the data turns up
  QVERIFY(!clipboard.has(Text));
  QVERIFY(!clipboard.takeLateFetch());

  // takeLateFetch() only reports the arrival once
  bool arrived = false;
  QTRY_VERIFY_WITH_TIMEOUT(arrived || (arrived = clipboard.takeLateFetch()), 10000);
  QVERIFY(clipboard.has(Text));
  QCOMPARE(clipboard.get(Text), std::string("late"));
  QVERIFY(!clipboard.takeLateFetch());
}

QTEST_MAIN(OSXClipboardTests)
