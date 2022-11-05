// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:typed_data';

import 'package:test/bootstrap/browser.dart';
import 'package:test/test.dart';

import 'package:ui/src/engine.dart';

void main() {
  internalBootstrapBrowserTest(() => testMain);
}

void testMain() {
  group('$SkiaFontCollection', () {
    final List<String> warnings = <String>[];
    late void Function(String) oldPrintWarning;

    setUpAll(() async {
      ensureFlutterViewEmbedderInitialized();
      await renderer.initialize();
      oldPrintWarning = printWarning;
      printWarning = (String warning) {
        warnings.add(warning);
      };
    });

    tearDownAll(() {
      printWarning = oldPrintWarning;
    });

    setUp(() {
      warnings.clear();
    });

    test('logs no warnings with the default mock asset manager', () async {
      final SkiaFontCollection fontCollection = SkiaFontCollection();
      final WebOnlyMockAssetManager mockAssetManager =
          WebOnlyMockAssetManager();
      await fontCollection.downloadAssetFonts(mockAssetManager);
      fontCollection.registerDownloadedFonts();

      expect(warnings, isEmpty);
    });

    test('logs a warning if one of the registered fonts is invalid', () async {
      final SkiaFontCollection fontCollection = SkiaFontCollection();
      final WebOnlyMockAssetManager mockAssetManager =
          WebOnlyMockAssetManager();
      mockAssetManager.defaultFontManifest = '''
[
   {
      "family":"Roboto",
      "fonts":[{"asset":"/fonts/Roboto-Regular.ttf"}]
   },
   {
      "family": "BrokenFont",
      "fonts":[{"asset":"packages/bogus/BrokenFont.ttf"}]
   }
  ]
      ''';
      // It should complete without error, but emit a warning about BrokenFont.
      await fontCollection.downloadAssetFonts(mockAssetManager);
      fontCollection.registerDownloadedFonts();
      expect(
        warnings,
        containsAllInOrder(
          <String>[
            'Failed to load font BrokenFont at packages/bogus/BrokenFont.ttf',
            'Verify that packages/bogus/BrokenFont.ttf contains a valid font.',
          ],
        ),
      );
    });

    test('prioritizes Ahem loaded via FontManifest.json', () async {
      final SkiaFontCollection fontCollection = SkiaFontCollection();
      final WebOnlyMockAssetManager mockAssetManager =
          WebOnlyMockAssetManager();
      mockAssetManager.defaultFontManifest = '''
        [
          {
            "family":"Ahem",
            "fonts":[{"asset":"/assets/fonts/Roboto-Regular.ttf"}]
          }
        ]
      '''.trim();

      final ByteBuffer robotoData = (await (await httpFetch('/assets/fonts/Roboto-Regular.ttf')).arrayBuffer())! as ByteBuffer;

      await fontCollection.downloadAssetFonts(mockAssetManager);
      await fontCollection.debugDownloadTestFonts();
      fontCollection.registerDownloadedFonts();
      expect(warnings, isEmpty);

      // Use `singleWhere` to make sure only one version of 'Ahem' is loaded.
      final RegisteredFont ahem = fontCollection.debugRegisteredFonts!
        .singleWhere((RegisteredFont font) => font.family == 'Ahem');

      // Check that the contents of 'Ahem' is actually Roboto, because that's
      // what's specified in the manifest, and the manifest takes precedence.
      expect(ahem.bytes.length, robotoData.lengthInBytes);
    });

    test('falls back to default Ahem URL', () async {
      final SkiaFontCollection fontCollection = SkiaFontCollection();
      final WebOnlyMockAssetManager mockAssetManager =
          WebOnlyMockAssetManager();
      mockAssetManager.defaultFontManifest = '[]';

      final ByteBuffer ahemData = (await (await httpFetch('/assets/fonts/ahem.ttf')).arrayBuffer())! as ByteBuffer;

      await fontCollection.downloadAssetFonts(mockAssetManager);
      await fontCollection.debugDownloadTestFonts();
      fontCollection.registerDownloadedFonts();
      expect(warnings, isEmpty);

      // Use `singleWhere` to make sure only one version of 'Ahem' is loaded.
      final RegisteredFont ahem = fontCollection.debugRegisteredFonts!
        .singleWhere((RegisteredFont font) => font.family == 'Ahem');

      // Check that the contents of 'Ahem' is actually Roboto, because that's
      // what's specified in the manifest, and the manifest takes precedence.
      expect(ahem.bytes.length, ahemData.lengthInBytes);
    });

    test('download fonts separately from registering', () async {
      final SkiaFontCollection fontCollection = SkiaFontCollection();

      await fontCollection.debugDownloadTestFonts();
      /// Fonts should have been downloaded, but not yet registered
      expect(fontCollection.debugRegisteredFonts, isEmpty);

      fontCollection.registerDownloadedFonts();
      /// Fonts should now be registered and _registeredFonts should be filled
      expect(fontCollection.debugRegisteredFonts, isNotEmpty);
      expect(warnings, isEmpty);
    });
  });
}
