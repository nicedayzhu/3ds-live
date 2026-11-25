import 'dart:async';

import 'package:flutter/services.dart';

/// Channel contract with the Kotlin receive engine.
///
/// Methods are commands; events are a broadcast stream of maps. Keeping the
/// contract in one file makes the Kotlin and Dart sides easy to audit.
class StreamBridge {
  StreamBridge({MethodChannel? method, EventChannel? events})
      : _method = method ?? const MethodChannel('3ds_live/stream'),
        _events = events ?? const EventChannel('3ds_live/events');

  final MethodChannel _method;
  final EventChannel _events;

  Future<void> start(Map<String, dynamic> settings) =>
      _method.invokeMethod<void>('start', settings);

  Future<void> stop() => _method.invokeMethod<void>('stop');

  Future<void> configure(Map<String, dynamic> settings) =>
      _method.invokeMethod<void>('configure', settings);

  Future<void> setLayout(String layout) =>
      _method.invokeMethod<void>('setLayout', layout);

  Future<String?> capture() => _method.invokeMethod<String>('capture');

  Future<void> setKeepScreenOn(bool on) =>
      _method.invokeMethod<void>('setKeepScreenOn', on);

  Future<String?> startRecording() =>
      _method.invokeMethod<String>('startRecording');

  Future<void> stopRecording() => _method.invokeMethod<void>('stopRecording');

  Future<int?> textureId() => _method.invokeMethod<int>('textureId');

  Future<Map<String, dynamic>?> exportDiagnostics() async {
    final result =
        await _method.invokeMapMethod<String, dynamic>('exportDiagnostics');
    return result;
  }

  Future<String?> loadSettings() => _method.invokeMethod<String>('loadSettings');

  Future<void> saveSettings(String json) =>
      _method.invokeMethod<void>('saveSettings', json);

  Stream<Map<String, dynamic>> get events => _events
      .receiveBroadcastStream()
      .map((event) => Map<String, dynamic>.from(event as Map));
}
