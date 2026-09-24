#pragma once
// ─────────────────────────────────────────────────────────────
// np_net.h — Capa de red y sesión de NeuralPlay
//   · Sesión única por dispositivo (token + guardia de expulsión)
//   · Helpers HTTP (GET, descarga con redirect a Spaces, POST JSON/form)
// Extraído de Main.cpp sin cambios de comportamiento.
// ─────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include <functional>
#include <atomic>
#if JUCE_IOS || JUCE_MAC
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <cstring>
#endif

// ── Sesión única por dispositivo ──
// La app manda la cabecera "X-Session-Token". Si el servidor la invalida (se
// inició sesión en otro dispositivo) responde 401 con error "sesion_reemplazada":
// disparamos npOnSessionKicked una sola vez (la UI cierra sesión y avisa).
static juce::String npSessionToken;
static std::function<void (juce::String)> npOnSessionKicked;
static std::atomic<bool> npKickAvisado { false };

static juce::String npAuthHeaders (const juce::String& token)
{
    juce::String h = "Authorization: Bearer " + token;
    if (npSessionToken.isNotEmpty()) h << "\r\nX-Session-Token: " << npSessionToken;
    return h;
}

static void npCheckKick (int status, const juce::String& body)
{
    if (status == 401 && body.contains ("sesion_reemplazada"))
    {
        if (! npKickAvisado.exchange (true))
        {
            juce::String msg = juce::String::fromUTF8 (
                "Se inici\xc3\xb3 sesi\xc3\xb3n en otro dispositivo. Por seguridad, cada "
                "cuenta solo puede estar activa en un dispositivo a la vez.");
            juce::MessageManager::callAsync ([msg] { if (npOnSessionKicked) npOnSessionKicked (msg); });
        }
    }
}

static juce::String httpGet (const juce::String& url, const juce::String& token)
{
    juce::URL u (url);
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withExtraHeaders (npAuthHeaders (token))
                    .withConnectionTimeoutMs (15000)
                    .withStatusCode (&status);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return {};
    auto body = in->readEntireStreamAsString();
    npCheckKick (status, body);
    return body;
}
static bool httpDownload (const juce::String& url, const juce::String& token, const juce::File& dest,
                          std::function<void (double)> onProgress = {},
                          std::function<bool()> cancel = {})
{
    // Paso 1: conectar al servidor SIN seguir el redirect. Así, si el audio vive en
    // Spaces (respuesta 302 con URL presignada), NO arrastramos el header "Authorization:
    // Bearer" al saltar a Spaces (S3 rechaza mezclar presignada + header de auth => 400).
    juce::WebInputStream probe (juce::URL (url), false);
    probe.withExtraHeaders (npAuthHeaders (token))
         .withConnectionTimeout (30000)
         .withNumRedirectsToFollow (0);
    if (! probe.connect (nullptr)) return false;

    juce::WebInputStream* in = &probe;                 // por defecto leemos del servidor (archivo local, 200)
    std::unique_ptr<juce::WebInputStream> spaces;      // si hubo redirect: stream de Spaces
    const int sc = probe.getStatusCode();
    if (sc >= 300 && sc < 400)
    {
        const juce::String loc = probe.getResponseHeaders().getValue ("Location", {});
        if (loc.isEmpty()) return false;
        // Paso 2: bajar de la URL presignada SIN header de autorización (se autentica sola).
        spaces.reset (new juce::WebInputStream (juce::URL (loc), false));
        spaces->withConnectionTimeout (30000).withNumRedirectsToFollow (5);
        if (! spaces->connect (nullptr)) return false;
        in = spaces.get();
    }
    else if (sc >= 400)
    {
        return false;
    }

    const juce::int64 expected = in->getTotalLength();   // -1 si el server no da Content-Length
    dest.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (dest);
    juce::int64 written = 0;
    {
        std::unique_ptr<juce::FileOutputStream> out (tmp.getFile().createOutputStream());
        if (out == nullptr) return false;
        juce::HeapBlock<char> buf (1 << 16);
        while (! in->isExhausted())
        {
            if (cancel && cancel()) return false;               // salida rápida (cierre de la app)
            const int got = in->read (buf, 1 << 16);
            if (got <= 0) break;
            out->write (buf, (size_t) got);
            written += got;
            if (onProgress && expected > 0) onProgress ((double) written / (double) expected);
        }
    }
    // Regla estricta: solo confirmamos el archivo si la descarga se completó de verdad.
    //  - Con Content-Length: exigir que se hayan escrito exactamente esos bytes.
    //  - Sin Content-Length (expected<=0): exigir que el stream llegó a EOF real (isExhausted),
    //    no que simplemente se cortó la lectura. Así una caída de red NUNCA deja media canción.
    const bool reachedEnd = in->isExhausted();
    if (expected > 0) { if (written != expected)               return false; }
    else              { if (! reachedEnd || written <= 0)      return false; }
    return tmp.overwriteTargetFileWithTemporary();
}

static juce::String httpPostJson (const juce::String& url, const juce::String& jsonBody)
{
    juce::URL u = juce::URL (url).withPOSTData (jsonBody);
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withExtraHeaders ("Content-Type: application/json")
                    .withConnectionTimeoutMs (8000)
                    .withStatusCode (&status);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return {};
    return in->readEntireStreamAsString();
}

// POST con parámetros de formulario + token en header Bearer (para /api/live/setlist/* y render)
static juce::String httpPostForm (const juce::String& baseUrl, juce::StringPairArray params, const juce::String& token)
{
    juce::URL u (baseUrl);
    params.set ("_", "1");   // asegura que JUCE use método POST (cuerpo no vacío)
    for (auto& k : params.getAllKeys()) u = u.withParameter (k, params[k]);
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withExtraHeaders (npAuthHeaders (token))
                    .withConnectionTimeoutMs (10000)
                    .withStatusCode (&status);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return {};
    auto body = in->readEntireStreamAsString();
    npCheckKick (status, body);
    return body;
}

// ── IP de la LAN y permiso de red local (para el modo En Vivo) ──
static juce::String localLanIp()
{
   #if JUCE_IOS || JUCE_MAC
    // Elegir la interfaz de Wi-Fi EXACTA (en0), no la de datos móviles (pdp_ip0).
    // En el iPhone la IP 10.x puede ser el CGNAT del operador → el iPad no la alcanza.
    struct ifaddrs* ifaddr = nullptr;
    juce::String wifi, other;
    if (getifaddrs (&ifaddr) == 0)
    {
        for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (! (ifa->ifa_flags & IFF_UP)) continue;
            char buf[INET_ADDRSTRLEN] = { 0 };
            auto* sin = (struct sockaddr_in*) ifa->ifa_addr;
            inet_ntop (AF_INET, &sin->sin_addr, buf, sizeof (buf));
            const juce::String ip (buf), name (ifa->ifa_name);
            if (ip == "127.0.0.1" || ip.startsWith ("169.254.")) continue;
            if (name.startsWith ("en")) { wifi = ip; break; }                  // en0/en1 = Wi-Fi (lo mejor)
            if (other.isEmpty() && ! name.startsWith ("pdp") && ! name.startsWith ("lo")) other = ip;
        }
        freeifaddrs (ifaddr);
    }
    if (wifi.isNotEmpty())  return wifi;
    if (other.isNotEmpty()) return other;
   #endif
    // Respaldo (otras plataformas): primera IPv4 no-loopback / no link-local.
    for (auto& a : juce::IPAddress::getAllAddresses (false))
    {
        if (a.isNull()) continue;
        const auto s = a.toString();
        if (s != "127.0.0.1" && ! s.startsWith ("169.254.")) return s;
    }
    return "127.0.0.1";
}

// En iOS, un servidor TCP entrante NO dispara por sí solo el aviso de "Red local",
// así que iOS bloquea las conexiones del iPad en silencio. Enviar un datagrama a la
// red local (multicast mDNS) fuerza a iOS a pedir el permiso → una vez otorgado,
// el servidor de NeuralPlay queda accesible desde NeuralCharts.
static void npTriggerLocalNetworkPermission()
{
   #if JUCE_IOS
    juce::DatagramSocket s (true);          // permitir broadcast/multicast
    s.bindToPort (0);
    const char* msg = "neuralsync";
    s.write ("224.0.0.251", 5353, msg, (int) std::strlen (msg));   // mDNS: la LAN
    s.write ("255.255.255.255", 5353, msg, (int) std::strlen (msg));
   #endif
}
