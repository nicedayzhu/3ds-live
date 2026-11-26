import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'version.dart';

class UpdateInfo {
  const UpdateInfo({
    required this.version,
    required this.url,
    this.notes = '',
  });

  final String version;
  final String url;
  final String notes;
}

bool isNewerVersion(String current, String candidate) {
  final a = _versionParts(current);
  final b = _versionParts(candidate);
  for (var i = 0; i < 3; i++) {
    if (b[i] != a[i]) return b[i] > a[i];
  }
  return false;
}

List<int> _versionParts(String value) {
  final parts = value
      .replaceFirst(RegExp(r'^[vV]'), '')
      .split(RegExp(r'[^0-9]'))
      .where((part) => part.isNotEmpty)
      .take(3)
      .map(int.tryParse)
      .toList();
  return [
    ...parts.whereType<int>(),
    for (var i = parts.length; i < 3; i++) 0,
  ].take(3).toList();
}

Future<UpdateInfo?> checkForUpdate() async {
  final client = HttpClient()
    ..connectionTimeout = const Duration(seconds: 5);
  try {
    final request = await client.getUrl(
      Uri.parse('https://api.github.com/repos/$appRepository/releases/latest'),
    );
    request.headers.set(
      HttpHeaders.userAgentHeader,
      '3DS-Live-Android/$appVersion',
    );
    request.headers.set(HttpHeaders.acceptHeader, 'application/vnd.github+json');
    final response = await request.close().timeout(const Duration(seconds: 8));
    if (response.statusCode != HttpStatus.ok) return null;
    final body = await response
        .transform(utf8.decoder)
        .join()
        .timeout(const Duration(seconds: 8));
    final data = jsonDecode(body);
    if (data is! Map<String, dynamic>) return null;
    final tag = (data['tag_name'] as String? ?? '').trim();
    final version = tag.replaceFirst(RegExp(r'^[vV]'), '');
    final url = (data['html_url'] as String? ?? appReleasesUrl).trim();
    if (version.isEmpty || !isNewerVersion(appVersion, version)) return null;
    return UpdateInfo(
      version: version,
      url: url.isEmpty ? appReleasesUrl : url,
      notes: (data['body'] as String? ?? '').trim(),
    );
  } catch (_) {
    // Update checks are best-effort; offline use must stay unaffected.
    return null;
  } finally {
    client.close(force: true);
  }
}
