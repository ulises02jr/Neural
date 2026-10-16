// np_keychain.cpp — Implementación del guardado seguro (Keychain iOS/macOS).
//
// IMPORTANTE: los headers de Security/CoreFoundation van ANTES que JUCE. Así el
// tipo 'Point' de Carbon (MacTypes.h) queda definido antes de que JUCE traiga
// juce::Point, evitando el error "reference to 'Point' is ambiguous".
#if defined(__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
 #include <Security/SecBase.h>
 #include <Security/SecItem.h>
#elif defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <wincred.h>
#endif

#include "np_keychain.h"
#include <cstring>

namespace npkc
{
#if defined(__APPLE__)
    static const char* kService = "com.neuralworship.play";

    static CFMutableDictionaryRef baseQuery (const juce::String& key)
    {
        CFMutableDictionaryRef q = CFDictionaryCreateMutable (kCFAllocatorDefault, 0,
                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue (q, kSecClass, kSecClassGenericPassword);
        CFStringRef svc = CFStringCreateWithCString (kCFAllocatorDefault, kService, kCFStringEncodingUTF8);
        CFStringRef acc = CFStringCreateWithCString (kCFAllocatorDefault, key.toRawUTF8(), kCFStringEncodingUTF8);
        CFDictionarySetValue (q, kSecAttrService, svc);
        CFDictionarySetValue (q, kSecAttrAccount, acc);
        CFRelease (svc);
        CFRelease (acc);
        return q;
    }

    bool set (const juce::String& key, const juce::String& value)
    {
        // Borrar cualquier entrada previa (más robusto que SecItemUpdate).
        { CFMutableDictionaryRef del = baseQuery (key); SecItemDelete (del); CFRelease (del); }

        CFMutableDictionaryRef add = baseQuery (key);
        const char* utf8 = value.toRawUTF8();
        CFDataRef data = CFDataCreate (kCFAllocatorDefault, (const UInt8*) utf8,
                                       (CFIndex) std::strlen (utf8));
        CFDictionarySetValue (add, kSecValueData, data);
        // Accesible solo con el dispositivo desbloqueado y sin migrar a otros equipos.
        CFDictionarySetValue (add, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
        OSStatus st = SecItemAdd (add, nullptr);
        CFRelease (data);
        CFRelease (add);
        return st == errSecSuccess;
    }

    juce::String get (const juce::String& key)
    {
        CFMutableDictionaryRef q = baseQuery (key);
        CFDictionarySetValue (q, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue (q, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef out = nullptr;
        OSStatus st = SecItemCopyMatching (q, &out);
        CFRelease (q);
        juce::String result;
        if (st == errSecSuccess && out != nullptr)
        {
            CFDataRef d = (CFDataRef) out;
            result = juce::String::fromUTF8 ((const char*) CFDataGetBytePtr (d),
                                             (int) CFDataGetLength (d));
        }
        if (out != nullptr) CFRelease (out);
        return result;
    }

    void remove (const juce::String& key)
    {
        CFMutableDictionaryRef q = baseQuery (key);
        SecItemDelete (q);
        CFRelease (q);
    }
#elif defined(_WIN32)
    // ── Windows Credential Manager (bóveda de credenciales del usuario) ──
    // Guardado seguro por usuario, persiste entre reinicios y no requiere admin.
    static juce::String targetName (const juce::String& key)
    {
        return "com.neuralworship.play/" + key;   // nombre único por credencial
    }

    bool set (const juce::String& key, const juce::String& value)
    {
        const juce::String target = targetName (key);
        const char* utf8 = value.toRawUTF8();
        const DWORD blobSize = (DWORD) std::strlen (utf8);

        CREDENTIALW cred;
        ZeroMemory (&cred, sizeof (cred));
        cred.Type            = CRED_TYPE_GENERIC;
        cred.TargetName      = const_cast<LPWSTR> (target.toWideCharPointer());
        cred.CredentialBlobSize = blobSize;
        cred.CredentialBlob  = (LPBYTE) utf8;
        cred.Persist         = CRED_PERSIST_LOCAL_MACHINE;   // por usuario, persistente
        return CredWriteW (&cred, 0) == TRUE;
    }

    juce::String get (const juce::String& key)
    {
        const juce::String target = targetName (key);
        PCREDENTIALW pcred = nullptr;
        juce::String result;
        if (CredReadW (target.toWideCharPointer(), CRED_TYPE_GENERIC, 0, &pcred)
            && pcred != nullptr)
        {
            if (pcred->CredentialBlob != nullptr && pcred->CredentialBlobSize > 0)
                result = juce::String::fromUTF8 ((const char*) pcred->CredentialBlob,
                                                 (int) pcred->CredentialBlobSize);
        }
        if (pcred != nullptr)
            CredFree (pcred);
        return result;
    }

    void remove (const juce::String& key)
    {
        const juce::String target = targetName (key);
        CredDeleteW (target.toWideCharPointer(), CRED_TYPE_GENERIC, 0);
    }
#else
    // Otras plataformas (Android/Linux): sin almacén seguro por ahora.
    bool         set    (const juce::String&, const juce::String&) { return false; }
    juce::String get    (const juce::String&) { return {}; }
    void         remove (const juce::String&) {}
#endif
}
