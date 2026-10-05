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

QTEST_MAIN(OSXClipboardTests)
