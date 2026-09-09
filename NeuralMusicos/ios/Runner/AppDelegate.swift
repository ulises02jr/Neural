import Flutter
import UIKit
import AVFoundation
import WebKit

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  private let audio = MultiTrackAudio()

  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)
    guard let messenger = engineBridge.pluginRegistry
      .registrar(forPlugin: "NeuralChannels")?.messenger() else { return }
    _registrarCanales(messenger)
  }

  private func _registrarCanales(_ messenger: FlutterBinaryMessenger) {
    // Canal de audio del Modo Ensayo.
    let audioCh = FlutterMethodChannel(name: "neural/audio", binaryMessenger: messenger)
    audioCh.setMethodCallHandler { [weak self] call, result in
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
      case "play": self.audio.play(from: args["offset"] as? Double ?? 0); result(nil)
      case "pause": self.audio.pause(); result(nil)
      case "seek": self.audio.seek(to: args["offset"] as? Double ?? 0); result(nil)
      case "setVolume":
        if let id = args["id"] as? String, let v = args["value"] as? Double { self.audio.setVolume(id, Float(v)) }
        result(nil)
      case "position": result(self.audio.positionSec())
      case "isPlaying": result(self.audio.isPlaying)
      case "stop": self.audio.stop(); result(nil)
      case "cacheSize": result(self.audio.cacheBytes())
      case "clearCache": self.audio.clearCache(); result(nil)
      default: result(FlutterMethodNotImplemented)
      }
    }

    // Canal general: abrir URL, en vivo (WebView) y preferencias.
    let appCh = FlutterMethodChannel(name: "neural/app", binaryMessenger: messenger)
    appCh.setMethodCallHandler { call, result in
      let args = call.arguments as? [String: Any] ?? [:]
      switch call.method {
      case "openUrl":
        if let u = args["url"] as? String, let url = URL(string: u) { UIApplication.shared.open(url) }
        result(nil)
      case "openLive":
        if let u = args["url"] as? String, let url = URL(string: u) { AppDelegate._mostrarLive(url) }
        result(nil)
      case "prefsGet":
        result((try? String(contentsOf: AppDelegate.prefsFile, encoding: .utf8)) ?? "{}")
      case "prefsSet":
        if let s = args["json"] as? String {
          try? s.write(to: AppDelegate.prefsFile, atomically: true, encoding: .utf8)
        }
        result(nil)
      default: result(FlutterMethodNotImplemented)
      }
    }
  }

  static var prefsFile: URL {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
    return base.appendingPathComponent("neural_prefs.json")
  }

  // Muestra el sistema en vivo (LAN) en un WebView modal DENTRO de la app.
  private static func _mostrarLive(_ url: URL) {
    guard let top = _topVC() else { return }
    let vc = LiveWebVC(url: url)
    let nav = UINavigationController(rootViewController: vc)
    nav.modalPresentationStyle = .fullScreen
    top.present(nav, animated: true)
  }

  private static func _topVC() -> UIViewController? {
    let scene = UIApplication.shared.connectedScenes.first { $0.activationState == .foregroundActive } as? UIWindowScene
    var vc = scene?.keyWindow?.rootViewController
    while let p = vc?.presentedViewController { vc = p }
    return vc
  }
}

/// Pantalla WebView para el sistema en vivo por LAN.
class LiveWebVC: UIViewController {
  private let url: URL
  init(url: URL) { self.url = url; super.init(nibName: nil, bundle: nil) }
  required init?(coder: NSCoder) { fatalError() }

  override func viewDidLoad() {
    super.viewDidLoad()
    title = "En vivo"
    view.backgroundColor = .black
    let wv = WKWebView(frame: view.bounds)
    wv.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    view.addSubview(wv)
    wv.load(URLRequest(url: url))
    navigationItem.leftBarButtonItem =
      UIBarButtonItem(barButtonSystemItem: .done, target: self, action: #selector(cerrar))
  }
  @objc private func cerrar() { dismiss(animated: true) }
}

/// Motor de audio multipista (AVAudioEngine). Igual que en macOS.
class MultiTrackAudio {
  private let engine = AVAudioEngine()
  private var players: [String: AVAudioPlayerNode] = [:]
  private var files: [String: AVAudioFile] = [:]
  private var volumes: [String: Float] = [:]
  private var durationSec: Double = 0
  private var playing = false
  private var startOffsetSec: Double = 0
  private var startDate: Date? = nil

  private func activarSesion() {
    // En iOS hay que activar la sesion de audio para reproducir.
    let s = AVAudioSession.sharedInstance()
    try? s.setCategory(.playback, mode: .default)
    try? s.setActive(true)
  }

  func load(_ stems: [[String: Any]], completion: @escaping (Result<Double, Error>) -> Void) {
    stop()
    engine.stop()
    for (_, p) in players { engine.detach(p) }
    players.removeAll(); files.removeAll(); volumes.removeAll()
    durationSec = 0
    activarSesion()

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
      if FileManager.default.fileExists(atPath: dest.path) {
        do { try usar(id, dest) } catch { lock.lock(); firstError = firstError ?? error; lock.unlock() }
        continue
      }
      group.enter()
      URLSession.shared.dataTask(with: url) { data, _, err in
        defer { group.leave() }
        if let err = err { lock.lock(); firstError = firstError ?? err; lock.unlock(); return }
        guard let data = data else { return }
        do { try data.write(to: dest); try usar(id, dest) }
        catch {
          try? FileManager.default.removeItem(at: dest)
          lock.lock(); firstError = firstError ?? error; lock.unlock()
        }
      }.resume()
    }

    group.notify(queue: .main) {
      if self.files.isEmpty { completion(.failure(firstError ?? NSError(domain: "audio", code: -1))); return }
      for (id, file) in self.files {
        let player = AVAudioPlayerNode()
        self.engine.attach(player)
        self.engine.connect(player, to: self.engine.mainMixerNode, format: file.processingFormat)
        player.volume = self.volumes[id] ?? 1.0
        self.players[id] = player
      }
      do { try self.engine.start(); completion(.success(self.durationSec)) }
      catch { completion(.failure(error)) }
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
    if !engine.isRunning { activarSesion(); try? engine.start() }
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

  private var cacheDir: URL {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    let dir = base.appendingPathComponent("ensayo_cache", isDirectory: true)
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir
  }

  func cacheBytes() -> Int {
    let fm = FileManager.default
    guard let items = try? fm.contentsOfDirectory(at: cacheDir, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
    var total = 0
    for u in items { if let sz = (try? u.resourceValues(forKeys: [.fileSizeKey]))?.fileSize { total += sz } }
    return total
  }

  func clearCache() {
    stop()
    let fm = FileManager.default
    if let items = try? fm.contentsOfDirectory(at: cacheDir, includingPropertiesForKeys: nil) {
      for u in items { try? fm.removeItem(at: u) }
    }
  }
}
