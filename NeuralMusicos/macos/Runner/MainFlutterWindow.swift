import Cocoa
import FlutterMacOS
import AVFoundation

class MainFlutterWindow: NSWindow {
  private let audio = MultiTrackAudio()

  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    RegisterGeneratedPlugins(registry: flutterViewController)

    // Canal del Modo Ensayo: motor de audio multipista nativo.
    let channel = FlutterMethodChannel(
      name: "neural/audio",
      binaryMessenger: flutterViewController.engine.binaryMessenger)
    channel.setMethodCallHandler { [weak self] call, result in
      guard let self = self else { return }
      let args = call.arguments as? [String: Any] ?? [:]
      switch call.method {
      case "load":
        let stems = args["stems"] as? [[String: Any]] ?? []
        self.audio.load(stems) { r in
          switch r {
          case .success(let dur): result(dur)
          case .failure(let e): result(FlutterError(code: "load", message: e.localizedDescription, details: nil))
          }
        }
      case "play":
        self.audio.play(from: args["offset"] as? Double ?? 0); result(nil)
      case "pause":
        self.audio.pause(); result(nil)
      case "seek":
        self.audio.seek(to: args["offset"] as? Double ?? 0); result(nil)
      case "setVolume":
        if let id = args["id"] as? String, let v = args["value"] as? Double {
          self.audio.setVolume(id, Float(v))
        }
        result(nil)
      case "position":
        result(self.audio.positionSec())
      case "isPlaying":
        result(self.audio.isPlaying)
      case "stop":
        self.audio.stop(); result(nil)
      case "cacheSize":
        result(self.audio.cacheBytes())
      case "clearCache":
        self.audio.clearCache(); result(nil)
      default:
        result(FlutterMethodNotImplemented)
      }
    }

    super.awakeFromNib()
  }
}

/// Motor de audio multipista para el Modo Ensayo.
/// Un solo AVAudioEngine con un AVAudioPlayerNode por stem, todos programados
/// desde el mismo frame para lograr sincronizacion de muestra.
class MultiTrackAudio {
  private let engine = AVAudioEngine()
  private var players: [String: AVAudioPlayerNode] = [:]
  private var files: [String: AVAudioFile] = [:]
  private var volumes: [String: Float] = [:]

  private var durationSec: Double = 0
  private var playing = false
  private var startOffsetSec: Double = 0
  private var startDate: Date? = nil

  func load(_ stems: [[String: Any]], completion: @escaping (Result<Double, Error>) -> Void) {
    stop()
    engine.stop()
    for (_, p) in players { engine.detach(p) }
    players.removeAll(); files.removeAll(); volumes.removeAll()
    durationSec = 0

    let dir = cacheDir
    let group = DispatchGroup()
    var firstError: Error? = nil
    let lock = NSLock()

    func usar(_ id: String, _ fileURL: URL) throws {
      let file = try AVAudioFile(forReading: fileURL)
      lock.lock()
      self.files[id] = file
      self.volumes[id] = 1.0
      let dur = Double(file.length) / file.processingFormat.sampleRate
      if dur > self.durationSec { self.durationSec = dur }
      lock.unlock()
    }

    for stem in stems {
      guard let id = stem["id"] as? String, let urlStr = stem["url"] as? String,
            let url = URL(string: urlStr) else { continue }
      let key = (stem["key"] as? String) ?? id
      let dest = dir.appendingPathComponent(key)

      // Si ya esta en cache, usarla sin descargar.
      if FileManager.default.fileExists(atPath: dest.path) {
        do { try usar(id, dest) } catch { lock.lock(); firstError = firstError ?? error; lock.unlock() }
        continue
      }
      group.enter()
      URLSession.shared.dataTask(with: url) { data, _, err in
        defer { group.leave() }
        if let err = err { lock.lock(); firstError = firstError ?? err; lock.unlock(); return }
        guard let data = data else { return }
        do {
          try data.write(to: dest)
          try usar(id, dest)
        } catch {
          try? FileManager.default.removeItem(at: dest)
          lock.lock(); firstError = firstError ?? error; lock.unlock()
        }
      }.resume()
    }

    group.notify(queue: .main) {
      if self.files.isEmpty {
        completion(.failure(firstError ?? NSError(domain: "audio", code: -1)))
        return
      }
      for (id, file) in self.files {
        let player = AVAudioPlayerNode()
        self.engine.attach(player)
        self.engine.connect(player, to: self.engine.mainMixerNode, format: file.processingFormat)
        player.volume = self.volumes[id] ?? 1.0
        self.players[id] = player
      }
      do {
        try self.engine.start()
        completion(.success(self.durationSec))
      } catch {
        completion(.failure(error))
      }
    }
  }

  private func scheduleAll(from offsetSec: Double) {
    for (id, file) in files {
      guard let player = players[id] else { continue }
      player.stop()
      let sr = file.processingFormat.sampleRate
      let startFrame = AVAudioFramePosition(max(0, offsetSec) * sr)
      let total = file.length
      if startFrame >= total { continue }
      let count = AVAudioFrameCount(total - startFrame)
      player.scheduleSegment(file, startingFrame: startFrame, frameCount: count, at: nil, completionHandler: nil)
    }
  }

  func play(from offsetSec: Double) {
    if !engine.isRunning { try? engine.start() }
    scheduleAll(from: offsetSec)
    let startAt = AVAudioTime(hostTime: mach_absolute_time() &+ Self.hostTicks(0.12))
    for (_, player) in players { player.play(at: startAt) }
    playing = true
    startOffsetSec = max(0, offsetSec)
    startDate = Date().addingTimeInterval(0.12)
  }

  private static func hostTicks(_ seconds: Double) -> UInt64 {
    var info = mach_timebase_info_data_t()
    mach_timebase_info(&info)
    let nanos = seconds * 1_000_000_000.0
    return UInt64(nanos * Double(info.denom) / Double(info.numer))
  }

  func pause() {
    guard playing else { return }
    startOffsetSec = positionSec()
    for (_, p) in players { p.pause() }
    playing = false
    startDate = nil
  }

  func seek(to offsetSec: Double) {
    let wasPlaying = playing
    for (_, p) in players { p.stop() }
    startOffsetSec = max(0, min(offsetSec, durationSec))
    if wasPlaying { play(from: startOffsetSec) } else { startDate = nil }
  }

  func stop() {
    for (_, p) in players { p.stop() }
    playing = false
    startOffsetSec = 0
    startDate = nil
  }

  func setVolume(_ id: String, _ value: Float) {
    volumes[id] = value
    players[id]?.volume = value
  }

  func positionSec() -> Double {
    if playing, let s = startDate {
      return min(startOffsetSec + Date().timeIntervalSince(s), durationSec)
    }
    return startOffsetSec
  }

  var isPlaying: Bool { playing }

  // MARK: - Cache persistente (offline)

  /// Carpeta de la app donde quedan guardadas las pistas descargadas.
  private var cacheDir: URL {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    let dir = base.appendingPathComponent("ensayo_cache", isDirectory: true)
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir
  }

  /// Bytes totales ocupados por las pistas guardadas.
  func cacheBytes() -> Int {
    let fm = FileManager.default
    guard let items = try? fm.contentsOfDirectory(at: cacheDir, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
    var total = 0
    for u in items {
      if let sz = (try? u.resourceValues(forKeys: [.fileSizeKey]))?.fileSize { total += sz }
    }
    return total
  }

  /// Borra todas las pistas guardadas para liberar espacio.
  func clearCache() {
    stop()
    let fm = FileManager.default
    if let items = try? fm.contentsOfDirectory(at: cacheDir, includingPropertiesForKeys: nil) {
      for u in items { try? fm.removeItem(at: u) }
    }
  }
}
