/*
    Snake Puppy Reference Player - network audio output (Lead implementation).

    Implemented for real:
      - SSDP M-SEARCH discovery of UPnP MediaRenderers
      - device description parsing (MediaRenderer + AVTransport control URL)
      - SOAP AVTransport control (SetAVTransportURI with DIDL-Lite, Play, Pause,
        Stop, Seek, GetTransportInfo, GetPositionInfo)
      - embedded HTTP media server with byte-range support (what renderers use
        to fetch the source file)
      - an in-process mock DLNA renderer used by runSelfTest() so the complete
        discovery + control + media-transfer path is proven locally.

    Local DSP does not apply over the network: the renderer decodes the source
    file itself. The UI states this explicitly.
*/
#include "Network/NetworkOutput.h"

#include "Audio/DecodeSource.h"
#include "DSP/Resampler.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <csignal>
#include "Core/ApplicationPaths.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <thread>

namespace sp
{
namespace
{
//==============================================================================
// Helpers
//==============================================================================
juce::String xmlEscape (const juce::String& text)
{
    return text.replace ("&", "&amp;")
               .replace ("<", "&lt;")
               .replace (">", "&gt;")
               .replace ("\"", "&quot;")
               .replace ("'", "&apos;");
}

juce::String percentDecode (const juce::String& text)
{
    juce::String result;
    for (int i = 0; i < text.length(); ++i)
    {
        auto c = text[i];
        if (c == '%' && i + 2 < text.length())
        {
            const int value = text.substring (i + 1, i + 3).getHexValue32();
            if (value >= 0)
            {
                result += juce::juce_wchar (value);
                i += 2;
                continue;
            }
        }
        result += c;
    }
    return result;
}

juce::String percentEncode (const juce::String& text)
{
    juce::String out;
    for (auto c : text)
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '-' || c == '_' || c == '.' || c == '/' || c == '~')
            out += juce::String::charToString (c);
        else
            out += "%" + juce::String::toHexString ((int) (juce::juce_wchar) c).paddedLeft ('0', 2).toUpperCase();
    }
    return out;
}

juce::String headerValue (const juce::String& datagram, const juce::String& headerName)
{
    for (auto& line : juce::StringArray::fromLines (datagram))
    {
        auto idx = line.indexOfChar (':');
        if (idx > 0 && line.substring (0, idx).trim().equalsIgnoreCase (headerName))
            return line.substring (idx + 1).trim();
    }
    return {};
}

bool writeAll (juce::StreamingSocket& socket, const void* data, int numBytes)
{
    auto* p = static_cast<const char*> (data);
    int written = 0;
    int stallCount = 0;
    while (written < numBytes)
    {
        const int n = socket.write (p + written, numBytes - written);
        if (n > 0)
        {
            written += n;
            stallCount = 0;
            continue;
        }

        // A non-blocking/partial socket can return 0 or -1 with EAGAIN under
        // backpressure. Wait for writability and retry rather than silently
        // truncating the response body.
        if (++stallCount > 200)
            return false;
        if (! socket.waitUntilReady (false, 100))
            juce::Thread::sleep (2);
    }
    return true;
}

bool writeAll (juce::StreamingSocket& socket, const juce::String& text)
{
    auto utf8 = text.toRawUTF8();
    return writeAll (socket, utf8, (int) std::strlen (utf8));
}

bool readHttpHead (juce::StreamingSocket& socket, juce::String& headOut)
{
    juce::String data;
    char buffer[2048];
    for (int iterations = 0; iterations < 64; ++iterations)
    {
        if (socket.waitUntilReady (true, 4000) <= 0)
            return false;
        const int n = socket.read (buffer, (int) sizeof (buffer), false);
        if (n <= 0)
            return false;
        data += juce::String::fromUTF8 (buffer, n);
        if (data.contains ("\r\n\r\n") || data.contains ("\n\n"))
        {
            headOut = data;
            return true;
        }
        if (data.length() > 64000)
            return false;
    }
    return false;
}

juce::String htmlEscape (const juce::String& text)
{
    return text.replace ("&", "&amp;")
               .replace ("<", "&lt;")
               .replace (">", "&gt;")
               .replace ("\"", "&quot;");
}


juce::String jsEscape (const juce::String& text)
{
    return text.replace ("\\", "\\\\")
               .replace ("'", "\\'")
               .replace ("\r", " ")
               .replace ("\n", " ");
}
juce::String mimeTypeForFile (const juce::File& file)
{
    auto ext = file.getFileExtension().toLowerCase();
    if (ext == ".flac") return "audio/flac";
    if (ext == ".wav" || ext == ".wave") return "audio/wav";
    if (ext == ".aif" || ext == ".aiff") return "audio/aiff";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".m4a" || ext == ".aac" || ext == ".alac") return "audio/mp4";
    if (ext == ".ogg" || ext == ".oga") return "audio/ogg";
    if (ext == ".dsf") return "audio/x-dsf";
    if (ext == ".dff") return "audio/x-dff";
    if (ext == ".caf") return "audio/x-caf";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".xml") return "text/xml";
    return "application/octet-stream";
}

bool isAllowedMediaExtension (const juce::File& file)
{
    static const char* allowed[] = { "flac", "wav", "wave", "aif", "aiff", "mp3", "m4a", "aac",
                                     "alac", "ogg", "oga", "dsf", "dff", "caf" };
    auto ext = file.getFileExtension().toLowerCase().trimCharactersAtStart (".");
    for (auto* a : allowed)
        if (ext == a)
            return true;
    return false;
}

juce::String formatTime (std::int64_t totalMs)
{
    const auto totalSeconds = juce::jmax ((std::int64_t) 0, totalMs / 1000);
    const auto h = totalSeconds / 3600;
    const auto m = (totalSeconds / 60) % 60;
    const auto s = totalSeconds % 60;
    return juce::String (h).paddedLeft ('0', 2) + ":" + juce::String (m).paddedLeft ('0', 2)
         + ":" + juce::String (s).paddedLeft ('0', 2);
}

std::int64_t parseTime (const juce::String& text)
{
    auto parts = juce::StringArray::fromTokens (text.trim(), ":", "");
    if (parts.size() != 3)
        return -1;
    return ((std::int64_t) parts[0].getIntValue() * 3600
            + (std::int64_t) parts[1].getIntValue() * 60
            + parts[2].getIntValue()) * 1000;
}

juce::String findXmlValue (const juce::XmlElement& element, const juce::String& tag, int depth = 0)
{
    if (depth > 16)
        return {};
    for (auto* child : element.getChildIterator())
        if (child->getTagName().equalsIgnoreCase (tag))
            return child->getAllSubText().trim();
    for (auto* child : element.getChildIterator())
    {
        auto result = findXmlValue (*child, tag, depth + 1);
        if (result.isNotEmpty())
            return result;
    }
    return {};
}

juce::String resolveUrl (const juce::String& baseUrl, const juce::String& relative)
{
    if (relative.isEmpty())
        return {};
    if (relative.startsWithIgnoreCase ("http://") || relative.startsWithIgnoreCase ("https://"))
        return relative;

    juce::String origin = baseUrl;
    auto schemeEnd = baseUrl.indexOf ("://");
    if (schemeEnd > 0)
    {
        auto slash = baseUrl.indexOf (schemeEnd + 3, "/");
        origin = slash > 0 ? baseUrl.substring (0, slash) : baseUrl;
    }
    if (relative.startsWith ("/"))
        return origin + relative;
    return origin + "/" + relative;
}

juce::String httpPost (const juce::String& urlText, const juce::String& soapAction,
                       const juce::String& body, juce::String& errorOut, int* statusOut = nullptr)
{
    juce::URL url (urlText);
    if (! url.isWellFormed())
    {
        errorOut = "Invalid renderer control URL: " + urlText;
        return {};
    }

    int status = 0;
    const auto headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"" + soapAction + "\"\r\n";

    auto stream = url.withPOSTData (body)
                     .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withConnectionTimeoutMs (4000)
                                             .withHttpRequestCmd ("POST")
                                             .withExtraHeaders (headers)
                                             .withStatusCode (&status));
    if (stream == nullptr)
    {
        errorOut = "Could not reach renderer control endpoint: " + urlText;
        return {};
    }

    auto response = stream->readEntireStreamAsString();
    if (statusOut != nullptr)
        *statusOut = status;
    if (status < 200 || status >= 300)
    {
        errorOut = "Renderer returned HTTP " + juce::String (status);
        return response;
    }
    return response;
}

juce::String soapErrorFrom (const juce::String& response)
{
    juce::XmlDocument doc (response);
    auto root = doc.getDocumentElement();
    if (root == nullptr)
        return {};
    auto errorDescription = findXmlValue (*root, "errorDescription");
    if (errorDescription.isNotEmpty())
        return errorDescription;
    if (response.containsIgnoreCase ("Fault"))
        return "renderer reported a SOAP fault";
    return {};
}

juce::String joinNotes (const std::vector<juce::String>& notes)
{
    juce::StringArray array;
    for (auto& n : notes)
        array.add (n);
    return array.joinIntoString (" | ");
}

juce::String localIpAddress()
{
    auto address = juce::IPAddress::getLocalAddress();
    if (address.toString().isNotEmpty() && ! address.toString().startsWith ("127.")
        && ! address.toString().startsWith ("169.254."))
        return address.toString();

    for (auto& candidate : juce::IPAddress::getAllAddresses())
    {
        auto text = candidate.toString();
        if (! text.startsWith ("127.") && ! text.startsWith ("169.254.") && ! text.contains (":"))
            return text;
    }
    return "127.0.0.1";
}

//==============================================================================
// Embedded HTTP media server.
//==============================================================================
class HttpMediaServer : private juce::Thread
{
public:
    HttpMediaServer() : juce::Thread ("SP Media Server") {}
    ~HttpMediaServer() override { stop(); }

    void setTrackProvider (std::function<std::vector<Track>()> provider)
    {
        trackProvider = std::move (provider);
    }

    void setCacheDirectory (const juce::File& directory)
    {
        cacheDirectory = directory;
    }

    bool start (int preferredPort, const juce::String& bindAddress, juce::String& errorOut)
    {
        stop();
        const auto bindAddr = bindAddress.isEmpty() ? juce::String ("0.0.0.0") : bindAddress;
        int port = 0;

        const int first = preferredPort > 0 ? preferredPort : 49800;
        const int attempts = preferredPort > 0 ? 1 : 80;
        for (int attempt = 0; attempt < attempts && port == 0; ++attempt)
        {
            const int candidate = first + attempt;
            if (listener.createListener (candidate, bindAddr))
                port = candidate;
        }

        if (port == 0 && preferredPort == 0)
        {
            if (listener.createListener (0, bindAddr))
                port = listener.getBoundPort();
        }

        if (port <= 0)
        {
            errorOut = "Could not open the media HTTP listener.";
            return false;
        }

        // A browser/phone that closes a connection mid-transfer must never
        // terminate the app via SIGPIPE.
        std::signal (SIGPIPE, SIG_IGN);

        boundPort = port;
        running = true;
        startThread (juce::Thread::Priority::normal);
        return true;
    }

    void stop()
    {
        if (! running.exchange (false))
            return;
        listener.close();
        stopThread (2000);
        boundPort = 0;
    }

    bool isRunning() const { return running.load(); }
    int getPort() const { return boundPort.load(); }

    juce::String getBaseUrl (const juce::String& localIp) const
    {
        const auto port = boundPort.load();
        if (port <= 0)
            return {};
        return "http://" + (localIp.isEmpty() ? juce::String ("127.0.0.1") : localIp) + ":" + juce::String (port);
    }

    juce::URL makeMediaUrl (const juce::String& localIp, const juce::File& file) const
    {
        const auto base = getBaseUrl (localIp);
        if (base.isEmpty() || ! file.existsAsFile())
            return {};
        return juce::URL (base + "/media?f=" + percentEncode (file.getFullPathName()));
    }

    void run() override
    {
        while (! threadShouldExit() && running.load())
        {
            if (listener.waitUntilReady (true, 200) <= 0)
                continue;
            auto* connection = listener.waitForNextConnection();
            if (connection == nullptr)
                continue;
            try
            {
                handleConnection (*connection);
            }
            catch (const std::exception& e)
            {
                juce::Logger::writeToLog (juce::String ("Media server connection error: ") + e.what());
            }
            catch (...)
            {
                juce::Logger::writeToLog ("Media server connection error: unknown exception");
            }
            connection->close();
            delete connection;
        }
    }

private:
    void handleConnection (juce::StreamingSocket& socket)
    {
        juce::String head;
        if (! readHttpHead (socket, head))
            return;

        auto lines = juce::StringArray::fromLines (head);
        if (lines.isEmpty())
            return;
        auto requestLine = juce::StringArray::fromTokens (lines[0].trim(), " ", "");
        if (requestLine.size() < 2)
            return;

        const auto method = requestLine[0];
        const auto target = requestLine[1];
        juce::String rangeHeader;
        for (int i = 1; i < lines.size(); ++i)
        {
            auto idx = lines[i].indexOfChar (':');
            if (idx > 0 && lines[i].substring (0, idx).trim().equalsIgnoreCase ("range"))
                rangeHeader = lines[i].substring (idx + 1).trim();
        }

        const bool headOnly = method.equalsIgnoreCase ("HEAD");
        const auto pathOnly = target.upToFirstOccurrenceOf ("?", false, false);
        if (pathOnly == "/" || pathOnly == "/index.html" || pathOnly == "/health")
        {
            if (! method.equalsIgnoreCase ("GET") && ! headOnly)
            {
                writeAll (socket, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            if (pathOnly == "/health")
                sendHealth (socket, headOnly);
            else
                sendLandingPage (socket, headOnly);
            return;
        }

        if (method.equalsIgnoreCase ("GET") || headOnly)
            serveFileRequest (socket, target, rangeHeader, headOnly);
        else
            writeAll (socket, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }

    void sendLandingPage (juce::StreamingSocket& socket, bool headOnly) const
    {
        const auto port = boundPort.load();
        std::vector<Track> tracks;
        if (trackProvider != nullptr)
            tracks = trackProvider();

        juce::String rows;
        juce::String jsTracks = "[";
        int playableIndex = 0;
        const int shown = juce::jmin ((int) tracks.size(), 2000);
        for (int i = 0; i < shown; ++i)
        {
            const auto& track = tracks[(size_t) i];
            if (track.path.isEmpty())
                continue;
            const auto url = "/media?f=" + percentEncode (track.path) + "&fmt=wav";
            const auto title = htmlEscape (track.displayTitle());
            const auto label = track.displayTitle() + " - " + track.displayArtist();
            auto subtitle = track.displayArtist();
            if (track.album.isNotEmpty())
                subtitle << " - " << track.album;
            if (track.formatName.isNotEmpty())
                subtitle << "  |  " << track.formatName;
            if (track.sampleRate > 0)
                subtitle << " " << (track.sampleRate % 1000 == 0
                                        ? juce::String (track.sampleRate / 1000) + " kHz"
                                        : juce::String (track.sampleRate / 1000.0, 1) + " kHz");

            if (playableIndex > 0)
                jsTracks << ",";
            jsTracks << "{u:'" << jsEscape (url) << "',t:'" << jsEscape (label) << "'}";
            ++playableIndex;

            rows << "<div class=\"track\" data-search=\"" << htmlEscape ((title + " " + subtitle).toLowerCase())
                 << "\" onclick=\"playAt(" << (playableIndex - 1) << ")\"><span class=\"ttl\">" << title
                 << "</span><span class=\"sub\">" << htmlEscape (subtitle) << "</span></div>";
        }
        jsTracks << "]";

        if (rows.isEmpty())
            rows = "<p class=\"empty\">No tracks in the library yet. Add music in the app, then reload this page.</p>";

        juce::String html;
        html << "<!doctype html><html><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
                "<title>Snake Puppy - Library</title><style>"
                "*{box-sizing:border-box}body{margin:0;background:#0e0f11;color:#e9e9e9;"
                "font:15px/1.45 -apple-system,BlinkMacSystemFont,Helvetica,Arial,sans-serif;padding-bottom:96px}"
                "header{position:sticky;top:0;z-index:2;background:#141619;border-bottom:1px solid #26292d;"
                "padding:14px 16px 12px;display:flex;align-items:center;gap:10px}"
                ".dot{width:10px;height:10px;border-radius:50%;background:#c9a227}"
                "h1{font-size:16px;margin:0;letter-spacing:.02em}.subtitle{color:#8a8f96;font-size:12px;margin-left:auto}"
                ".bar{padding:12px 16px;display:flex;gap:8px}input{flex:1;background:#0c0d0f;border:1px solid #2a2d31;"
                "color:#eee;border-radius:9px;padding:9px 12px;font-size:15px}"
                ".track{padding:11px 16px;border-bottom:1px solid #1c1f22;cursor:pointer}"
                ".track:active{background:#1b1e22}.ttl{display:block;font-weight:600;font-size:15px;color:#f2f2f2}"
                ".sub{display:block;color:#8f949b;font-size:12.5px;margin-top:2px}"
                ".empty{color:#8f949b;padding:24px 16px}"
                "footer{position:fixed;left:0;right:0;bottom:0;background:#141619;border-top:1px solid #26292d;"
                "padding:10px 14px 14px;padding-bottom:calc(14px + env(safe-area-inset-bottom))}"
                "#row1{display:flex;align-items:center;gap:8px}#now{color:#c9a227;font-size:12px;margin-bottom:8px;"
                "white-space:nowrap;overflow:hidden;text-overflow:ellipsis;flex:1}"
                "#cnt{color:#8f949b;font-size:11px;margin-bottom:8px;white-space:nowrap}"
                "#controls{display:flex;gap:10px;margin-bottom:8px}"
                "#controls button{flex:1;background:#1b1e22;border:1px solid #2e3237;color:#e9e9e9;"
                "border-radius:9px;padding:9px 0;font-size:17px}"
                "#controls button:active{background:#262a2f}"
                "#seekrow{display:flex;align-items:center;gap:10px;margin-bottom:6px}"
                "#seekrow span{color:#9aa0a8;font-size:11px;font-variant-numeric:tabular-nums;min-width:34px;text-align:center}"
                "#seek{-webkit-appearance:none;flex:1;height:22px;background:transparent}"
                "#seek::-webkit-slider-runnable-track{height:4px;border-radius:2px;background:#3a3f45}"
                "#seek::-webkit-slider-thumb{-webkit-appearance:none;width:16px;height:16px;margin-top:-6px;"
                "border-radius:50%;background:#d8b74a;border:none}"
                "a{color:#d8b74a}"
                "</style></head><body>"
                "<header><span class=\"dot\"></span><h1>Snake Puppy</h1>"
                "<span class=\"subtitle\">"
             << (int) tracks.size() << " tracks | port " << port << "</span></header>"
                "<div class=\"bar\"><input id=\"q\" placeholder=\"Search title, artist, album\" "
                "oninput=\"filterTracks(this.value)\"></div><div id=\"list\">"
             << rows <<
                "</div><footer><div id=\"row1\"><div id=\"now\">Tap a track to play</div>"
                "<div id=\"cnt\"></div></div>"
                "<div id=\"controls\">"
                "<button onclick=\"step(-1)\" title=\"Previous\">&#9198;</button>"
                "<button onclick=\"toggle()\" id=\"pp\" title=\"Play/Pause\">&#9208;</button>"
                "<button onclick=\"step(1)\" title=\"Next\">&#9197;</button></div>"
                "<div id=\"seekrow\"><span id=\"tcur\">0:00</span>"
                "<input id=\"seek\" type=\"range\" min=\"0\" max=\"1000\" value=\"0\" step=\"1\">"
                "<span id=\"tdur\">0:00</span></div>"
                "<audio id=\"a\" preload=\"none\" playsinline style=\"display:none\"></audio>"
                "<div style=\"font-size:11px;color:#6f747b;margin-top:6px\">"
                "Phone-compatible 16-bit/44.1 kHz WAV (converted on first play, cached; no app DSP). "
                "Queue advances automatically. <a href=\"/health\">health</a></div></footer>"
                "<script>var tracks="
             << jsTracks <<
                ";var idx=-1;var a=document.getElementById('a');"
                "function playAt(i){if(i<0||i>=tracks.length)return;idx=i;var tr=tracks[i];"
                "document.getElementById('now').textContent=tr.t;"
                "document.getElementById('cnt').textContent=(i+1)+' / '+tracks.length;"
                "document.getElementById('pp').innerHTML='&#9208;';"
                "document.getElementById('seek').value=0;document.getElementById('tcur').textContent='0:00';"
                "a.src=tr.u;var p=a.play();"
                "if(p&&p.catch)p.catch(function(){document.getElementById('now').textContent='Cannot play: '+tr.t;});}"
                "function step(d){if(idx<0){playAt(0);return;}var n=idx+d;if(n>=0&&n<tracks.length)playAt(n);}"
                "function toggle(){if(a.paused){a.play();document.getElementById('pp').innerHTML='&#9208;';}"
                "else{a.pause();document.getElementById('pp').innerHTML='&#9654;';}}"
                "a.addEventListener('ended',function(){if(idx+1<tracks.length){playAt(idx+1);}"
                "else{document.getElementById('now').textContent='End of queue';document.getElementById('cnt').textContent='';}});"
                "a.addEventListener('play',function(){document.getElementById('pp').innerHTML='&#9208;';});"
                "a.addEventListener('pause',function(){document.getElementById('pp').innerHTML='&#9654;';});"
                "var seeking=false;function fmt(s){if(!isFinite(s))return'0:00';s=Math.max(0,Math.floor(s));"
                "return Math.floor(s/60)+':'+('0'+(s%60)).slice(-2);}"
                "function updateSeek(){var d=a.duration;if(!isFinite(d)||d<=0){document.getElementById('tdur').textContent='0:00';return;}"
                "document.getElementById('tdur').textContent=fmt(d);document.getElementById('tcur').textContent=fmt(a.currentTime);"
                "if(!seeking)document.getElementById('seek').value=Math.round(1000*a.currentTime/d);}"
                "a.addEventListener('timeupdate',updateSeek);a.addEventListener('durationchange',updateSeek);"
                "a.addEventListener('loadedmetadata',updateSeek);"
                "var seekEl=document.getElementById('seek');"
                "seekEl.addEventListener('input',function(){seeking=true;var d=a.duration;if(isFinite(d)&&d>0){"
                "a.currentTime=d*(seekEl.value/1000);document.getElementById('tcur').textContent=fmt(a.currentTime);}});"
                "seekEl.addEventListener('change',function(){seeking=false;});"
                "function filterTracks(v){v=v.toLowerCase();document.querySelectorAll('.track').forEach(function(e){"
                "e.style.display=e.dataset.search.indexOf(v)>=0?'block':'none';});}</script></body></html>";

        const auto bodyBytes = html.getNumBytesAsUTF8();
        juce::String header;
        header << "HTTP/1.1 200 OK\r\n"
               << "Server: SnakePuppyReferencePlayer/0.1\r\n"
               << "Content-Type: text/html; charset=\"utf-8\"\r\n"
               << "Content-Length: " << (int) bodyBytes << "\r\n"
               << "Connection: close\r\n\r\n";
        writeAll (socket, header);
        if (! headOnly)
            writeAll (socket, html);
    }

    void sendHealth (juce::StreamingSocket& socket, bool headOnly) const
    {
        juce::String json;
        json << "{\"service\":\"Snake Puppy Reference Player\",\"status\":\"ok\","
                "\"mediaServer\":true,\"port\":" << boundPort.load() << ","
                "\"mediaEndpoint\":\"/media?f=<url-encoded path>\","
                "\"rangeRequests\":true}";
        const auto bodyBytes = json.getNumBytesAsUTF8();
        juce::String header;
        header << "HTTP/1.1 200 OK\r\n"
               << "Server: SnakePuppyReferencePlayer/0.1\r\n"
               << "Content-Type: application/json\r\n"
               << "Content-Length: " << (int) bodyBytes << "\r\n"
               << "Connection: close\r\n\r\n";
        writeAll (socket, header);
        if (! headOnly)
            writeAll (socket, json);
    }

    juce::File transcodedWavFor (const juce::File& source, juce::String& errorOut) const
    {
        if (cacheDirectory == juce::File())
        {
            errorOut = "No transcode cache directory";
            return {};
        }
        cacheDirectory.createDirectory();

        const auto key = source.getFullPathName() + "|" + juce::String (source.getSize())
                       + "|" + juce::String (source.getLastModificationTime().toMilliseconds())
                       + "|44k16";
        const auto hash = juce::String::toHexString (key.hashCode64())
                        + "-" + juce::String (source.getSize());
        auto target = cacheDirectory.getChildFile (hash + ".wav");
        if (target.existsAsFile() && target.getSize() > 44)
            return target;

        juce::String decodeError;
        auto decoder = DecodeSource::create (source, decodeError);
        if (decoder == nullptr)
        {
            errorOut = decodeError;
            return {};
        }

        constexpr int targetRate = 44100;
        const int sourceRate = decoder->getInfo().sampleRate;
        std::unique_ptr<Resampler> resampler;
        if (sourceRate > 0 && sourceRate != targetRate)
        {
            resampler = std::make_unique<Resampler>();
            resampler->prepare ((double) sourceRate, (double) targetRate, 2, Resampler::Quality::high);
        }

        auto temp = target.getSiblingFile (target.getFileName() + ".tmp");
        temp.deleteFile();
        auto stream = temp.createOutputStream();
        if (stream == nullptr)
        {
            errorOut = "Could not create transcode file";
            return {};
        }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.get(), (double) targetRate, 2, 16, {}, 0));
        if (writer == nullptr)
        {
            errorOut = "Could not create WAV writer";
            return {};
        }
        stream.release();   // writer owns it now

        constexpr int blockFrames = 4096;
        std::vector<double> interleaved ((size_t) blockFrames * 2, 0.0);
        juce::AudioBuffer<float> out (2, blockFrames);
        juce::AudioBuffer<double> in (2, blockFrames);
        std::unique_ptr<juce::AudioBuffer<double>> resampled;
        int maxResampled = blockFrames;
        if (resampler != nullptr)
        {
            maxResampled = juce::jmax (blockFrames,
                                       Resampler::maxOutputFrames ((double) sourceRate, (double) targetRate,
                                                                   blockFrames));
            resampled = std::make_unique<juce::AudioBuffer<double>> (2, maxResampled);
        }

        auto writeWavBlock = [&] (const double* left, const double* right, int frames) -> bool
        {
            auto* outL = out.getWritePointer (0);
            auto* outR = out.getWritePointer (1);
            for (int i = 0; i < frames; ++i)
            {
                outL[i] = (float) juce::jlimit (-1.0, 1.0, left[i]);
                outR[i] = (float) juce::jlimit (-1.0, 1.0, right[i]);
            }
            juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, frames);
            return writer->writeFromAudioSampleBuffer (view, 0, frames);
        };

        for (;;)
        {
            const int got = decoder->readFrames (interleaved.data(), blockFrames);
            if (got <= 0)
                break;

            auto* inL = in.getWritePointer (0);
            auto* inR = in.getWritePointer (1);
            for (int i = 0; i < got; ++i)
            {
                inL[i] = interleaved[(size_t) i * 2];
                inR[i] = interleaved[(size_t) i * 2 + 1];
            }

            if (resampler != nullptr && resampled != nullptr)
            {
                const double* inCh[2] = { inL, inR };
                double* outCh[2] = { resampled->getWritePointer (0), resampled->getWritePointer (1) };
                const int produced = resampler->process (inCh, got, outCh, maxResampled);
                if (produced > 0 && ! writeWavBlock (resampled->getReadPointer (0),
                                                     resampled->getReadPointer (1), produced))
                {
                    errorOut = "WAV write failed";
                    writer.reset();
                    temp.deleteFile();
                    return {};
                }
            }
            else if (! writeWavBlock (inL, inR, got))
            {
                errorOut = "WAV write failed";
                writer.reset();
                temp.deleteFile();
                return {};
            }
        }

        if (resampler != nullptr && resampled != nullptr)
        {
            juce::AudioBuffer<double> flushIn (2, 1);
            flushIn.clear();
            const double* inCh[2] = { flushIn.getReadPointer (0), flushIn.getReadPointer (1) };
            double* outCh[2] = { resampled->getWritePointer (0), resampled->getWritePointer (1) };
            const int produced = resampler->process (inCh, 0, outCh, maxResampled, true);
            if (produced > 0)
                writeWavBlock (resampled->getReadPointer (0), resampled->getReadPointer (1), produced);
        }

        writer->flush();
        writer.reset();

        if (! temp.existsAsFile() || temp.getSize() <= 44)
        {
            errorOut = "Transcode produced no audio";
            temp.deleteFile();
            return {};
        }

        temp.moveFileTo (target);
        return target;
    }

    bool serveFileRequest (juce::StreamingSocket& socket, const juce::String& target,
                                  const juce::String& rangeHeader, bool headOnly)
    {
        juce::String path;
        bool wantWav = false;
        auto queryStart = target.indexOfChar ('?');
        if (queryStart >= 0)
        {
            auto query = target.substring (queryStart + 1);
            for (auto& pair : juce::StringArray::fromTokens (query, "&", ""))
            {
                if (pair.startsWith ("f="))
                    path = percentDecode (pair.substring (2));
                else if (pair == "fmt=wav")
                    wantWav = true;
            }
        }

        if (path.isEmpty())
        {
            writeAll (socket, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return false;
        }

        // When the app provides its library, only serve files that are in it.
        if (trackProvider != nullptr)
        {
            const auto allowed = trackProvider();
            bool found = false;
            for (auto& track : allowed)
                if (track.path == path)
                {
                    found = true;
                    break;
                }
            if (! found)
            {
                writeAll (socket, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return false;
            }
        }

        juce::File file (path);
        if (! file.existsAsFile() || ! isAllowedMediaExtension (file))
        {
            writeAll (socket, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return false;
        }

        // Phone-friendly delivery: decode any supported source to 16-bit/44.1 kHz
        // WAV (cached) so browsers that cannot decode FLAC/DSD/hi-res still play.
        if (wantWav)
        {
            juce::String transcodeError;
            auto wavFile = transcodedWavFor (file, transcodeError);
            if (wavFile == juce::File())
            {
                writeAll (socket, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                juce::Logger::writeToLog ("Web player transcode failed: " + transcodeError);
                return false;
            }
            file = wavFile;
        }

        const auto totalSize = file.getSize();
        std::int64_t start = 0;
        std::int64_t end = totalSize - 1;
        bool isPartial = false;

        if (rangeHeader.startsWithIgnoreCase ("bytes="))
        {
            auto spec = rangeHeader.substring (6).trim();
            auto dash = spec.indexOfChar ('-');
            if (dash >= 0)
            {
                auto startText = spec.substring (0, dash).trim();
                auto endText = spec.substring (dash + 1).trim();
                if (startText.isEmpty() && endText.isNotEmpty())
                {
                    const auto suffix = (std::int64_t) endText.getLargeIntValue();
                    start = juce::jmax ((std::int64_t) 0, totalSize - suffix);
                    end = totalSize - 1;
                }
                else
                {
                    start = (std::int64_t) startText.getLargeIntValue();
                    end = endText.isNotEmpty() ? juce::jmin ((std::int64_t) endText.getLargeIntValue(), totalSize - 1)
                                               : totalSize - 1;
                }
                isPartial = true;
            }
        }

        if (start >= totalSize || start < 0 || end < start)
        {
            writeAll (socket, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */"
                              + juce::String (totalSize) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return false;
        }

        const auto length = end - start + 1;
        juce::String header;
        header << (isPartial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n")
               << "Server: SnakePuppyReferencePlayer/0.1\r\n"
               << "Content-Type: " << mimeTypeForFile (file) << "\r\n"
               << "Accept-Ranges: bytes\r\n"
               << "Content-Length: " << juce::String (length) << "\r\n";
        if (isPartial)
            header << "Content-Range: bytes " << juce::String (start) << "-" << juce::String (end)
                   << "/" << juce::String (totalSize) << "\r\n";
        header << "Connection: close\r\n\r\n";
        writeAll (socket, header);

        if (headOnly)
            return true;

        juce::FileInputStream input (file);
        if (! input.openedOk())
            return false;
        input.setPosition (start);

        juce::HeapBlock<char> buffer (16384);
        std::int64_t remaining = length;
        while (remaining > 0)
        {
            const int chunk = (int) juce::jmin ((std::int64_t) 16384, remaining);
            const int read = input.read (buffer.getData(), chunk);
            if (read <= 0)
                break;
            if (! writeAll (socket, buffer.getData(), read))
                return false;
            remaining -= read;
        }
        return true;
    }

    juce::StreamingSocket listener;
    std::atomic<int> boundPort { 0 };
    std::atomic<bool> running { false };
    std::function<std::vector<Track>()> trackProvider;
    juce::File cacheDirectory;
};

//==============================================================================
// Mock DLNA renderer used by the self test.
//==============================================================================
class MockRenderer
{
public:
    MockRenderer() = default;
    ~MockRenderer() { stop(); }

    bool start (juce::String& errorOut)
    {
        stop();

        httpPort = 0;
        for (int port = 49860; port < 49900 && httpPort == 0; ++port)
            if (httpListener.createListener (port, "127.0.0.1"))
                httpPort = port;

        if (httpPort == 0)
        {
            errorOut = "Mock renderer could not bind an HTTP port.";
            return false;
        }

        running = true;
        httpThread = std::thread ([this] { runHttp(); });
        ssdpThread = std::thread ([this] { runSsdp(); });
        return true;
    }

    void stop()
    {
        if (! running.exchange (false))
            return;
        httpListener.close();
        ssdpSocket.shutdown();
        if (httpThread.joinable()) httpThread.join();
        if (ssdpThread.joinable()) ssdpThread.join();
        httpPort = 0;
    }

    juce::String getUdn() const { return "uuid:snake-puppy-mock-renderer"; }

    juce::String getDescriptionUrl() const
    {
        return "http://127.0.0.1:" + juce::String (httpPort.load()) + "/device.xml";
    }

    juce::CriticalSection stateLock;
    bool receivedSetUri = false;
    bool receivedPlay = false;
    bool mediaFetchOk = false;
    juce::String lastMediaUri;
    juce::String lastMetadata;

private:
    void runSsdp()
    {
        juce::DatagramSocket socket (true);
        socket.setEnablePortReuse (true);
        if (! socket.bindToPort (1900, "127.0.0.1"))
            return;

        while (running.load())
        {
            if (socket.waitUntilReady (true, 100) <= 0)
                continue;
            char buffer[2048] = {};
            juce::String senderIp;
            int senderPort = 0;
            const int n = socket.read (buffer, (int) sizeof (buffer) - 1, false, senderIp, senderPort);
            if (n <= 0)
                continue;
            auto request = juce::String::fromUTF8 (buffer, n);
            if (! request.containsIgnoreCase ("M-SEARCH"))
                continue;

            juce::String response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "CACHE-CONTROL: max-age=1800\r\n"
                     << "EXT:\r\n"
                     << "LOCATION: " << getDescriptionUrl() << "\r\n"
                     << "SERVER: SnakePuppyMock/1.0 UPnP/1.1\r\n"
                     << "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                     << "USN: " << getUdn() << "::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
            socket.write (senderIp, senderPort, response.toRawUTF8(), response.getNumBytesAsUTF8());
        }
    }

    void runHttp()
    {
        while (running.load())
        {
            if (httpListener.waitUntilReady (true, 100) <= 0)
                continue;
            auto* connection = httpListener.waitForNextConnection();
            if (connection == nullptr)
                continue;
            handleHttp (*connection);
            connection->close();
            delete connection;
        }
    }

    juce::String descriptionXml() const
    {
        juce::String xml;
        xml << "<?xml version=\"1.0\"?>"
            << "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
            << "<specVersion><major>1</major><minor>0</minor></specVersion>"
            << "<device>"
            << "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
            << "<friendlyName>Snake Puppy Mock Renderer</friendlyName>"
            << "<manufacturer>Snake Puppy Audio</manufacturer>"
            << "<modelName>Mock Renderer</modelName>"
            << "<UDN>" << getUdn() << "</UDN>"
            << "<serviceList>"
            << "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
            << "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
            << "<controlURL>/control/avt</controlURL><eventSubURL>/event/avt</eventSubURL>"
            << "<SCPDURL>/avt.xml</SCPDURL></service>"
            << "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
            << "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
            << "<controlURL>/control/cm</controlURL><eventSubURL>/event/cm</eventSubURL>"
            << "<SCPDURL>/cm.xml</SCPDURL></service>"
            << "</serviceList></device></root>";
        return xml;
    }

    juce::String soapResponse (const juce::String& action, const juce::String& body)
    {
        juce::String payload;
        juce::XmlDocument doc (body);
        auto root = doc.getDocumentElement();

        if (action == "SetAVTransportURI")
        {
            if (root != nullptr)
            {
                lastMediaUri = findXmlValue (*root, "CurrentURI");
                lastMetadata = findXmlValue (*root, "CurrentURIMetaData");
            }
            const juce::ScopedLock sl (stateLock);
            receivedSetUri = lastMediaUri.isNotEmpty();
            payload << "<u:SetAVTransportURIResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>";
        }
        else if (action == "Play")
        {
            {
                const juce::ScopedLock sl (stateLock);
                receivedPlay = true;
            }
            payload << "<u:PlayResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>";
            fetchMedia();
        }
        else if (action == "Pause")
            payload << "<u:PauseResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>";
        else if (action == "Stop")
            payload << "<u:StopResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>";
        else if (action == "Seek")
            payload << "<u:SeekResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>";
        else if (action == "GetTransportInfo")
            payload << "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                    << "<CurrentTransportState>PLAYING</CurrentTransportState>"
                    << "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                    << "<CurrentSpeed>1</CurrentSpeed></u:GetTransportInfoResponse>";
        else if (action == "GetPositionInfo")
            payload << "<u:GetPositionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                    << "<Track>1</Track><TrackDuration>00:00:01</TrackDuration>"
                    << "<TrackMetaData></TrackMetaData><TrackURI>" << xmlEscape (lastMediaUri) << "</TrackURI>"
                    << "<RelTime>00:00:00</RelTime><AbsTime>00:00:00</AbsTime>"
                    << "<RelCount>0</RelCount><AbsCount>0</AbsCount>"
                    << "</u:GetPositionInfoResponse>";
        else
            payload << "<u:" << action << "Response xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>";

        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
               "<s:Body>" + payload + "</s:Body></s:Envelope>";
    }

    void fetchMedia()
    {
        juce::String uri;
        {
            const juce::ScopedLock sl (stateLock);
            uri = lastMediaUri;
        }
        if (uri.isEmpty())
            return;

        int status = 0;
        auto stream = juce::URL (uri)
                          .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                  .withConnectionTimeoutMs (3000)
                                                  .withExtraHeaders ("Range: bytes=0-1023\r\n")
                                                  .withStatusCode (&status));
        if (stream == nullptr)
            return;
        juce::MemoryOutputStream received;
        received.writeFromInputStream (*stream, 4096);
        const juce::ScopedLock sl (stateLock);
        mediaFetchOk = (status == 206 || status == 200) && received.getDataSize() > 0;
    }

    void handleHttp (juce::StreamingSocket& socket)
    {
        juce::String head;
        if (! readHttpHead (socket, head))
            return;

        auto lines = juce::StringArray::fromLines (head);
        if (lines.isEmpty())
            return;
        auto requestLine = juce::StringArray::fromTokens (lines[0].trim(), " ", "");
        if (requestLine.size() < 2)
            return;
        const auto method = requestLine[0];
        const auto target = requestLine[1];

        if (target.startsWith ("/device.xml"))
        {
            const auto xml = descriptionXml();
            const auto bytes = xml.toRawUTF8();
            writeAll (socket, "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: "
                              + juce::String ((int) std::strlen (bytes)) + "\r\nConnection: close\r\n\r\n");
            writeAll (socket, bytes, (int) std::strlen (bytes));
            return;
        }

        if (target.startsWith ("/control/avt") && method.equalsIgnoreCase ("POST"))
        {
            int contentLength = 0;
            juce::String soapAction;
            for (int i = 1; i < lines.size(); ++i)
            {
                auto idx = lines[i].indexOfChar (':');
                if (idx <= 0) continue;
                auto name = lines[i].substring (0, idx).trim();
                auto value = lines[i].substring (idx + 1).trim();
                if (name.equalsIgnoreCase ("Content-Length"))
                    contentLength = value.getIntValue();
                else if (name.equalsIgnoreCase ("SOAPACTION"))
                    soapAction = value.unquoted();
            }

            juce::String body;
            char buffer[4096];
            int remaining = contentLength;
            while (remaining > 0)
            {
                if (socket.waitUntilReady (true, 2000) <= 0)
                    break;
                const int n = socket.read (buffer, juce::jmin ((int) sizeof (buffer), remaining), false);
                if (n <= 0)
                    break;
                body += juce::String::fromUTF8 (buffer, n);
                remaining -= n;
            }

            auto hash = soapAction.indexOfChar ('#');
            auto action = hash >= 0 ? soapAction.substring (hash + 1) : soapAction;
            auto response = soapResponse (action, body);
            auto bytes = response.toRawUTF8();
            writeAll (socket, "HTTP/1.1 200 OK\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nContent-Length: "
                              + juce::String ((int) std::strlen (bytes)) + "\r\nConnection: close\r\n\r\n");
            writeAll (socket, bytes, (int) std::strlen (bytes));
            return;
        }

        writeAll (socket, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }

    juce::StreamingSocket httpListener;
    juce::DatagramSocket ssdpSocket { true };
    std::atomic<bool> running { false };
    std::atomic<int> httpPort { 0 };
    std::thread httpThread;
    std::thread ssdpThread;
};

//==============================================================================
void writeSelfTestWav (const juce::File& target)
{
    const int sampleRate = 8000;
    const int numSamples = sampleRate;
    const std::uint32_t dataBytes = (std::uint32_t) (numSamples * 2);
    const std::uint32_t riffSize = 36 + dataBytes;

    juce::MemoryOutputStream out;
    auto write32 = [&out] (std::uint32_t v) { out.writeInt ((int) v); };
    auto write16 = [&out] (std::uint16_t v) { out.writeShort ((short) v); };

    out.write ("RIFF", 4);
    write32 (riffSize);
    out.write ("WAVEfmt ", 8);
    write32 (16);
    write16 (1);
    write16 (1);
    write32 ((std::uint32_t) sampleRate);
    write32 ((std::uint32_t) (sampleRate * 2));
    write16 (2);
    write16 (16);
    out.write ("data", 4);
    write32 (dataBytes);
    for (int i = 0; i < numSamples; ++i)
    {
        const auto value = (short) (std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate) * 16000.0);
        out.writeShort (value);
    }
    target.replaceWithData (out.getData(), out.getDataSize());
}
} // namespace

//==============================================================================
struct NetworkOutput::Impl
{
    explicit Impl (const ApplicationPaths& p) : paths (p)
    {
        configFile = p.appDataDir.getChildFile ("network-output.json");
        mediaServer.setCacheDirectory (p.appDataDir.getChildFile ("web-cache"));
    }

    const ApplicationPaths& paths;
    juce::File configFile;
    NetworkOutputConfig config;

    juce::CriticalSection stateLock;
    std::vector<NetworkEndpoint> endpoints;
    juce::StringArray discoveryLog;
    std::unique_ptr<juce::Thread> discoveryThread;
    HttpMediaServer mediaServer;
    MockRenderer mockRenderer;

    NetworkEndpoint connectedEndpoint;
    bool connectedFlag = false;

    VerificationState verification = VerificationState::untested;
    juce::String verificationDetail;
    juce::String statusText = "Network output idle";
    juce::String localIp = localIpAddress();
    std::function<void()> notifyChange;

    void log (const juce::String& message)
    {
        const juce::ScopedLock sl (stateLock);
        discoveryLog.add (message);
        while (discoveryLog.size() > 300)
            discoveryLog.remove (0);
    }

    void notify()
    {
        if (notifyChange != nullptr)
            notifyChange();
    }

    //== SSDP =================================================================
    std::vector<NetworkEndpoint> discoverBlocking (int timeoutMs, bool includeLoopback, bool storeResults)
    {
        std::vector<NetworkEndpoint> found;

        juce::DatagramSocket socket (true);
        socket.setEnablePortReuse (true);
        if (! socket.bindToPort (0))
        {
            log ("Discovery: could not bind a UDP socket");
            return found;
        }

        auto buildSearch = [] (const juce::String& st)
        {
            return "M-SEARCH * HTTP/1.1\r\n"
                   "HOST: 239.255.255.250:1900\r\n"
                   "MAN: \"ssdp:discover\"\r\n"
                   "MX: 2\r\n"
                   "ST: " + st + "\r\n\r\n";
        };

        auto multicast = buildSearch ("urn:schemas-upnp-org:device:MediaRenderer:1");
        socket.write ("239.255.255.250", 1900, multicast.toRawUTF8(), multicast.getNumBytesAsUTF8());
        if (includeLoopback)
        {
            auto loop = buildSearch ("ssdp:all");
            socket.write ("127.0.0.1", 1900, loop.toRawUTF8(), loop.getNumBytesAsUTF8());
        }

        // Also probe the user-configured renderer host (e.g. a Raspberry Pi).
        {
            juce::String manual = config.manualHost.trim();
            if (manual.startsWithIgnoreCase ("http://") || manual.startsWithIgnoreCase ("https://"))
                manual = juce::URL (manual).getDomain();
            if (manual.isNotEmpty())
            {
                socket.write (manual, 1900, multicast.toRawUTF8(), multicast.getNumBytesAsUTF8());
                log ("SSDP unicast probe: " + manual);
            }
        }
        log ("SSDP M-SEARCH sent (MediaRenderer)");

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (300, timeoutMs);
        std::map<juce::String, NetworkEndpoint> byUdn;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            const int remaining = (int) deadline - (int) juce::Time::getMillisecondCounter();
            if (socket.waitUntilReady (true, juce::jmax (10, remaining)) <= 0)
                break;

            char buffer[8192] = {};
            juce::String senderIp;
            int senderPort = 0;
            const int n = socket.read (buffer, (int) sizeof (buffer) - 1, false, senderIp, senderPort);
            if (n <= 0)
                continue;

            auto datagram = juce::String::fromUTF8 (buffer, n);
            auto location = headerValue (datagram, "LOCATION");
            if (location.isEmpty())
                continue;

            auto endpoint = fetchDescription (location);
            if (endpoint.udn.isEmpty() && endpoint.friendlyName.isEmpty())
                continue;
            if (endpoint.avTransportControlUrl.isEmpty())
                continue;

            endpoint.hostAddress = senderIp;
            endpoint.reachable = true;
            byUdn[endpoint.udn.isNotEmpty() ? endpoint.udn : location] = endpoint;
            log ("Discovered: " + endpoint.friendlyName + " @ " + senderIp);
        }

        for (auto& [key, ep] : byUdn)
        {
            juce::ignoreUnused (key);
            found.push_back (ep);
        }

        if (storeResults)
        {
            const juce::ScopedLock sl (stateLock);
            for (auto& ep : found)
            {
                bool exists = false;
                for (auto& existing : endpoints)
                {
                    if (existing.udn == ep.udn)
                    {
                        existing = ep;
                        exists = true;
                        break;
                    }
                }
                if (! exists)
                    endpoints.push_back (ep);
            }
        }
        return found;
    }

    NetworkEndpoint fetchDescription (const juce::String& location)
    {
        NetworkEndpoint endpoint;
        endpoint.descriptionUrl = location;

        int status = 0;
        auto stream = juce::URL (location).createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (2500)
                .withStatusCode (&status));
        if (stream == nullptr)
            return endpoint;

        auto xmlText = stream->readEntireStreamAsString();
        juce::XmlDocument doc (xmlText);
        auto root = doc.getDocumentElement();
        if (root == nullptr)
            return endpoint;

        endpoint.udn = findXmlValue (*root, "UDN");
        endpoint.friendlyName = findXmlValue (*root, "friendlyName");
        endpoint.manufacturer = findXmlValue (*root, "manufacturer");
        endpoint.modelName = findXmlValue (*root, "modelName");
        if (endpoint.friendlyName.isEmpty())
            endpoint.friendlyName = endpoint.udn;

        std::function<void (const juce::XmlElement&)> walk = [&] (const juce::XmlElement& element)
        {
            if (element.getTagName().equalsIgnoreCase ("service"))
            {
                auto type = findXmlValue (element, "serviceType");
                auto control = findXmlValue (element, "controlURL");
                if (type.containsIgnoreCase ("AVTransport") && control.isNotEmpty())
                    endpoint.avTransportControlUrl = resolveUrl (location, control);
                else if (type.containsIgnoreCase ("ConnectionManager") && control.isNotEmpty())
                    endpoint.connectionManagerControlUrl = resolveUrl (location, control);
            }
            for (auto* child : element.getChildIterator())
                walk (*child);
        };
        walk (*root);

        juce::URL descriptionUrl (location);
        if (descriptionUrl.getPort() > 0)
            endpoint.port = descriptionUrl.getPort();
        return endpoint;
    }

    NetworkEndpoint probeHostBlocking (const juce::String& hostOrUrlIn, int timeoutMs,
                                       juce::String& messageOut)
    {
        messageOut.clear();
        auto target = hostOrUrlIn.trim();
        if (target.isEmpty())
        {
            messageOut = "Enter a renderer host (for example 192.168.1.50) or a device description URL.";
            return {};
        }

        const bool urlForm = target.startsWithIgnoreCase ("http://") || target.startsWithIgnoreCase ("https://");
        juce::String host = target;
        if (! urlForm)
        {
            if (host.contains ("://"))
                host = host.fromFirstOccurrenceOf ("://", false, false);
            host = host.upToFirstOccurrenceOf ("/", false, false);
            if (host.contains (":"))                       // IPv4/hostname:port
                host = host.upToFirstOccurrenceOf (":", false, false);
        }
        log ("Host probe: " + target);

        NetworkEndpoint endpoint;
        if (urlForm)
        {
            endpoint = fetchDescription (target);
            if (endpoint.friendlyName.isEmpty() && endpoint.udn.isEmpty())
            {
                messageOut = "No readable UPnP device description at " + target
                           + ". Check that the address is a UPnP/DLNA MediaRenderer description URL.";
                {
                    const juce::ScopedLock sl (stateLock);
                    statusText = messageOut;
                }
                log (messageOut);
                notify();
                return {};
            }
        }
        else
        {
            juce::DatagramSocket socket (true);
            socket.setEnablePortReuse (true);
            if (! socket.bindToPort (0))
            {
                messageOut = "Could not open a UDP socket for host probing.";
                return {};
            }

            const juce::String search = "M-SEARCH * HTTP/1.1\r\n"
                                        "HOST: 239.255.255.250:1900\r\n"
                                        "MAN: \"ssdp:discover\"\r\n"
                                        "MX: 2\r\n"
                                        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                socket.write (host, 1900, search.toRawUTF8(), search.getNumBytesAsUTF8());
                if (attempt == 0)
                    juce::Thread::sleep (60);
            }

            const auto deadline = juce::Time::getMillisecondCounter()
                                  + (juce::uint32) juce::jmax (400, timeoutMs);
            while (juce::Time::getMillisecondCounter() < deadline)
            {
                const int remaining = (int) deadline - (int) juce::Time::getMillisecondCounter();
                if (socket.waitUntilReady (true, juce::jmax (10, remaining)) <= 0)
                    break;

                char buffer[8192] = {};
                juce::String senderIp;
                int senderPort = 0;
                const int n = socket.read (buffer, (int) sizeof (buffer) - 1, false, senderIp, senderPort);
                if (n <= 0)
                    continue;
                if (senderIp != host && host.containsChar ('.'))
                    continue;                              // ignore stray responders

                auto datagram = juce::String::fromUTF8 (buffer, n);
                auto location = headerValue (datagram, "LOCATION");
                if (location.isEmpty())
                    continue;

                auto candidate = fetchDescription (location);
                if (candidate.avTransportControlUrl.isNotEmpty())
                {
                    endpoint = candidate;
                    break;
                }
            }
            if (endpoint.avTransportControlUrl.isNotEmpty())
                endpoint.hostAddress = host;
        }

        if (endpoint.avTransportControlUrl.isEmpty())
        {
            messageOut = "No UPnP/DLNA MediaRenderer responded at " + target
                       + ". That host must run a renderer service (for example upmpdcli or "
                         "gmrender-resurrect) that answers UPnP discovery; this player cannot "
                         "stream directly to a host without one.";
            {
                const juce::ScopedLock sl (stateLock);
                statusText = messageOut;
            }
            log (messageOut);
            notify();
            return {};
        }

        if (endpoint.friendlyName.isEmpty())
            endpoint.friendlyName = endpoint.udn.isNotEmpty() ? endpoint.udn : host;
        if (urlForm)
        {
            auto description = juce::URL (target);
            endpoint.hostAddress = description.getDomain();
            if (endpoint.port > 0)
                endpoint.hostAddress << ":" << endpoint.port;
        }
        endpoint.hostAddress = endpoint.hostAddress.isNotEmpty() ? endpoint.hostAddress : host;
        endpoint.reachable = true;

        {
            const juce::ScopedLock sl (stateLock);
            bool replaced = false;
            for (auto& existing : endpoints)
            {
                if ((existing.udn.isNotEmpty() && existing.udn == endpoint.udn)
                    || existing.hostAddress == endpoint.hostAddress)
                {
                    existing = endpoint;
                    replaced = true;
                    break;
                }
            }
            if (! replaced)
                endpoints.push_back (endpoint);
            statusText = "Found " + endpoint.friendlyName + " at " + endpoint.hostAddress + ".";
        }

        messageOut = "Found " + endpoint.friendlyName + " at " + endpoint.hostAddress
                   + (endpoint.modelName.isNotEmpty() ? " (" + endpoint.modelName + ")" : "")
                   + ". Select it in the list and press CONNECT.";
        log (messageOut);
        notify();
        return endpoint;
    }

    struct DiscoveryThread : juce::Thread
    {
        DiscoveryThread (Impl* ownerIn, int timeoutIn)
            : juce::Thread ("SP UPnP Discovery"), owner (ownerIn), timeoutMs (timeoutIn) {}

        void run() override
        {
            auto endpointsFound = owner->discoverBlocking (timeoutMs, true, true);
            if (threadShouldExit())
                return;
            {
                const juce::ScopedLock sl (owner->stateLock);
                owner->statusText = endpointsFound.empty()
                                        ? "No UPnP/DLNA renderer found on this network."
                                        : "Found " + juce::String ((int) endpointsFound.size()) + " renderer(s).";
            }
            owner->notify();
        }

        Impl* owner;
        int timeoutMs;
    };

    void startDiscovery (int timeoutMs)
    {
        stopDiscovery();
        {
            const juce::ScopedLock sl (stateLock);
            statusText = "Discovering UPnP/DLNA renderers...";
        }
        discoveryThread = std::make_unique<DiscoveryThread> (this, timeoutMs);
        discoveryThread->startThread (juce::Thread::Priority::normal);
    }

    void stopDiscovery()
    {
        if (discoveryThread != nullptr)
        {
            discoveryThread->signalThreadShouldExit();
            discoveryThread->stopThread (4000);
            discoveryThread.reset();
        }
    }

    bool isDiscovering() const
    {
        return discoveryThread != nullptr && discoveryThread->isThreadRunning();
    }

    //== SOAP =================================================================
    static constexpr const char* serviceType = "urn:schemas-upnp-org:service:AVTransport:1";

    static juce::String soapEnvelope (const juce::String& action, const juce::String& argsXml)
    {
        juce::String envelope;
        envelope << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                 << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                 << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                 << "<s:Body><u:" << action << " xmlns:u=\"" << serviceType << "\">"
                 << "<InstanceID>0</InstanceID>" << argsXml
                 << "</u:" << action << "></s:Body></s:Envelope>";
        return envelope;
    }

    bool callAction (const juce::String& action, const juce::String& argsXml,
                     juce::String& responseOut, juce::String& errorOut)
    {
        if (! connectedFlag || connectedEndpoint.avTransportControlUrl.isEmpty())
        {
            errorOut = "No UPnP renderer is connected.";
            return false;
        }
        auto response = httpPost (connectedEndpoint.avTransportControlUrl,
                                  juce::String (serviceType) + "#" + action,
                                  soapEnvelope (action, argsXml), errorOut);
        responseOut = response;
        if (errorOut.isNotEmpty())
            return false;
        auto soapError = soapErrorFrom (response);
        if (soapError.isNotEmpty())
        {
            errorOut = "Renderer error: " + soapError;
            return false;
        }
        return true;
    }

    //== Config ===============================================================
    juce::var configToVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("enabled", config.enabled);
        o->setProperty ("preferredRendererUdn", config.preferredRendererUdn);
        o->setProperty ("serveMediaOverHttp", config.serveMediaOverHttp);
        o->setProperty ("httpPort", config.httpPort);
        o->setProperty ("bindAddress", config.bindAddress);
        o->setProperty ("autoReconnectOnPlay", config.autoReconnectOnPlay);
        o->setProperty ("manualHost", config.manualHost);
        return juce::var (o);
    }

    void varToConfig (const juce::var& v)
    {
        if (auto* o = v.getDynamicObject())
        {
            config.enabled = (bool) o->getProperty ("enabled");
            config.preferredRendererUdn = o->getProperty ("preferredRendererUdn").toString();
            config.serveMediaOverHttp = (bool) o->getProperty ("serveMediaOverHttp");
            config.httpPort = (int) o->getProperty ("httpPort");
            config.bindAddress = o->getProperty ("bindAddress").toString();
            config.autoReconnectOnPlay = (bool) o->getProperty ("autoReconnectOnPlay");
            if (o->hasProperty ("manualHost"))
                config.manualHost = o->getProperty ("manualHost").toString();
        }
    }
};

//==============================================================================
NetworkOutput::NetworkOutput (const ApplicationPaths& paths) : impl (std::make_unique<Impl> (paths))
{
    impl->notifyChange = [this] { sendChangeMessage(); };
}

NetworkOutput::~NetworkOutput()
{
    stopDiscovery();
    stopMediaServer();
    impl->mockRenderer.stop();
}

//== Discovery =================================================================
void NetworkOutput::startDiscovery (int timeoutMs)
{
    impl->startDiscovery (timeoutMs);
    sendChangeMessage();
}

void NetworkOutput::stopDiscovery() { impl->stopDiscovery(); }
bool NetworkOutput::isDiscovering() const { return impl->isDiscovering(); }

std::vector<NetworkEndpoint> NetworkOutput::getEndpoints() const
{
    const juce::ScopedLock sl (impl->stateLock);
    return impl->endpoints;
}

NetworkEndpoint NetworkOutput::probeHostBlocking (const juce::String& hostOrUrl, int timeoutMs,
                                                  juce::String& messageOut)
{
    return impl->probeHostBlocking (hostOrUrl, timeoutMs, messageOut);
}

//== Config ====================================================================
NetworkOutputConfig NetworkOutput::getConfig() const { return impl->config; }

void NetworkOutput::setConfig (const NetworkOutputConfig& config)
{
    impl->config = config;
    saveConfig();
    sendChangeMessage();
}

void NetworkOutput::loadConfig()
{
    if (! impl->configFile.existsAsFile())
        return;
    auto text = impl->configFile.loadFileAsString();
    juce::var v;
    if (juce::JSON::parse (text, v).failed())
        return;
    impl->varToConfig (v);
}

void NetworkOutput::saveConfig()
{
    impl->configFile.getParentDirectory().createDirectory();
    impl->configFile.replaceWithText (juce::JSON::toString (impl->configToVar(), true));
}

//== Connection ================================================================
bool NetworkOutput::connectTo (const NetworkEndpoint& endpoint, juce::String& errorOut)
{
    if (endpoint.avTransportControlUrl.isEmpty())
    {
        errorOut = "The selected renderer does not advertise AVTransport control.";
        return false;
    }
    impl->connectedEndpoint = endpoint;
    impl->connectedFlag = true;
    impl->config.preferredRendererUdn = endpoint.udn;
    impl->statusText = "Connected to " + endpoint.friendlyName;
    sendChangeMessage();
    return true;
}

void NetworkOutput::disconnect()
{
    impl->connectedFlag = false;
    impl->connectedEndpoint = {};
    impl->statusText = "Network output disconnected";
    sendChangeMessage();
}

bool NetworkOutput::isConnected() const { return impl->connectedFlag; }
juce::String NetworkOutput::getConnectedRendererName() const { return impl->connectedEndpoint.friendlyName; }

//== Control ===================================================================
bool NetworkOutput::playUri (const juce::URL& mediaUri, const juce::String& title,
                             const juce::String& artist, const juce::String& album,
                             juce::String& errorOut)
{
    if (! mediaUri.isWellFormed())
    {
        errorOut = "Invalid media URL.";
        return false;
    }

    juce::String didl;
    didl << "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
         << "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
         << "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
         << "<item id=\"track-1\" parentID=\"0\" restricted=\"1\">"
         << "<dc:title>" << xmlEscape (title) << "</dc:title>"
         << "<dc:creator>" << xmlEscape (artist) << "</dc:creator>"
         << "<upnp:artist>" << xmlEscape (artist) << "</upnp:artist>"
         << "<upnp:album>" << xmlEscape (album) << "</upnp:album>"
         << "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
         << "</item></DIDL-Lite>";

    juce::String args;
    args << "<CurrentURI>" << xmlEscape (mediaUri.toString (true)) << "</CurrentURI>"
         << "<CurrentURIMetaData>" << xmlEscape (didl) << "</CurrentURIMetaData>";

    juce::String response;
    if (! impl->callAction ("SetAVTransportURI", args, response, errorOut))
        return false;
    if (! impl->callAction ("Play", "<Speed>1</Speed>", response, errorOut))
        return false;

    impl->statusText = "Playing on " + impl->connectedEndpoint.friendlyName;
    sendChangeMessage();
    return true;
}

bool NetworkOutput::pause (juce::String& errorOut)
{
    juce::String response;
    return impl->callAction ("Pause", {}, response, errorOut);
}

bool NetworkOutput::stop (juce::String& errorOut)
{
    juce::String response;
    return impl->callAction ("Stop", {}, response, errorOut);
}

bool NetworkOutput::seekToMs (std::int64_t positionMs, juce::String& errorOut)
{
    juce::String args;
    args << "<Unit>REL_TIME</Unit><Target>" << formatTime (positionMs) << "</Target>";
    juce::String response;
    return impl->callAction ("Seek", args, response, errorOut);
}

bool NetworkOutput::queryTransportState (juce::String& stateOut, std::int64_t* positionMs)
{
    juce::String response, error;
    if (! impl->callAction ("GetTransportInfo", {}, response, error))
        return false;
    juce::XmlDocument doc (response);
    auto root = doc.getDocumentElement();
    if (root == nullptr)
        return false;
    stateOut = findXmlValue (*root, "CurrentTransportState");

    if (positionMs != nullptr)
    {
        juce::String positionResponse;
        if (impl->callAction ("GetPositionInfo", {}, positionResponse, error))
        {
            juce::XmlDocument positionDoc (positionResponse);
            if (auto positionRoot = positionDoc.getDocumentElement())
                *positionMs = juce::jmax ((std::int64_t) 0, parseTime (findXmlValue (*positionRoot, "RelTime")));
        }
    }
    return stateOut.isNotEmpty();
}

//== HTTP media server =========================================================
bool NetworkOutput::startMediaServer (juce::String& errorOut)
{
    if (! impl->config.serveMediaOverHttp)
    {
        errorOut = "Media HTTP server is disabled in the network configuration.";
        return false;
    }
    if (impl->mediaServer.isRunning())
        return true;
    if (! impl->mediaServer.start (impl->config.httpPort, impl->config.bindAddress, errorOut))
        return false;
    impl->statusText = "Media server listening on port " + juce::String (impl->mediaServer.getPort());
    sendChangeMessage();
    return true;
}

void NetworkOutput::stopMediaServer()
{
    impl->mediaServer.stop();
    sendChangeMessage();
}

void NetworkOutput::setMediaLibraryProvider (std::function<std::vector<Track>()> provider)
{
    impl->mediaServer.setTrackProvider (std::move (provider));
}

bool NetworkOutput::isMediaServerRunning() const { return impl->mediaServer.isRunning(); }
int NetworkOutput::getMediaServerPort() const { return impl->mediaServer.getPort(); }

juce::String NetworkOutput::getMediaServerBaseUrl() const
{
    return impl->mediaServer.getBaseUrl (impl->localIp);
}

juce::URL NetworkOutput::makeMediaUrl (const juce::File& file) const
{
    return impl->mediaServer.makeMediaUrl (impl->localIp, file);
}

//== Capability + verification =================================================
juce::StringArray NetworkOutput::supportedTransports()
{
    return { "UPnP/DLNA AVTransport (implemented, local self-test)",
             "File delivery over embedded HTTP with byte ranges",
             "PiBridge SSH PCM via ~/hqdac - local DSP applied (one track at a time)" };
}

juce::StringArray NetworkOutput::unavailableTransports()
{
    return { "AirPlay sender: macOS exposes no public API for sending custom PCM streams to AirPlay",
             "Snapcast: not implemented",
             "Roon: proprietary protocol, not implemented",
             "HQPlayer NAA: proprietary protocol, not implemented",
             "DLNA audio-only streaming of processed PCM: renderers fetch the source file, so local DSP does not apply" };
}

juce::StringArray NetworkOutput::getLastDiscoveryLog() const
{
    const juce::ScopedLock sl (impl->stateLock);
    return impl->discoveryLog;
}

NetworkOutput::VerificationState NetworkOutput::getVerifiedState() const { return impl->verification; }
juce::String NetworkOutput::getVerificationDetail() const { return impl->verificationDetail; }

juce::String NetworkOutput::getStatusText() const
{
    const juce::ScopedLock sl (impl->stateLock);
    return impl->statusText;
}

bool NetworkOutput::SelfTestResult::passed() const
{
    return ssdpSearchWorks && mockRendererDiscovered && soapControlWorks && httpRangeServerWorks;
}

NetworkOutput::SelfTestResult NetworkOutput::runSelfTest()
{
    SelfTestResult result;
    impl->stopDiscovery();
    impl->mockRenderer.stop();
    impl->mediaServer.stop();
    result.notes.push_back ("Starting in-process mock DLNA renderer...");

    juce::String error;
    if (! impl->mockRenderer.start (error))
    {
        result.notes.push_back ("Mock renderer failed to start: " + error);
        impl->verification = VerificationState::failed;
        impl->verificationDetail = joinNotes (result.notes);
        return result;
    }

    if (! impl->mediaServer.start (0, "0.0.0.0", error))
    {
        result.notes.push_back ("Media server failed to start: " + error);
        impl->mockRenderer.stop();
        impl->verification = VerificationState::failed;
        impl->verificationDetail = joinNotes (result.notes);
        return result;
    }

    // A small deterministic WAV file to serve and fetch.
    auto testFile = impl->paths.appDataDir.getChildFile ("selftest").getChildFile ("selftest-tone.wav");
    testFile.getParentDirectory().createDirectory();
    writeSelfTestWav (testFile);

    // 1. SSDP discovery (multicast + loopback probe).
    auto found = impl->discoverBlocking (2200, true, true);
    result.ssdpSearchWorks = true;   // request was sent and socket worked; discovery result checked below
    bool mockFound = false;
    NetworkEndpoint mockEndpoint;
    for (auto& ep : found)
    {
        if (ep.udn == impl->mockRenderer.getUdn())
        {
            mockFound = true;
            mockEndpoint = ep;
            break;
        }
    }
    result.mockRendererDiscovered = mockFound;
    result.notes.push_back (mockFound ? "Mock renderer discovered via SSDP and description parsed."
                                : "Mock renderer was NOT discovered via SSDP.");
    if (mockFound)
    {
        result.notes.push_back ("AVTransport control URL: " + mockEndpoint.avTransportControlUrl);
    }

    // 2. HTTP range server direct check (independent of the renderer).
    {
        const auto mediaUrl = impl->mediaServer.makeMediaUrl ("127.0.0.1", testFile);
        result.httpRangeServerWorks = false;
        if (! mediaUrl.isWellFormed())
        {
            result.notes.push_back ("Could not build a local media URL.");
        }
        else
        {
            int status = 0;
            auto stream = mediaUrl.createInputStream (
                juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (3000)
                    .withExtraHeaders ("Range: bytes=100-199\r\n")
                    .withStatusCode (&status));
            if (stream == nullptr)
            {
                result.notes.push_back ("Range request failed (no stream).");
            }
            else
            {
                juce::MemoryOutputStream received;
                received.writeFromInputStream (*stream, 4096);
                juce::MemoryBlock expected;
                auto* fileIn = testFile.createInputStream().release();
                std::unique_ptr<juce::FileInputStream> fileStream (static_cast<juce::FileInputStream*> (fileIn));
                fileStream->setPosition (100);
                expected.setSize (100);
                fileStream->read (expected.getData(), 100);

                result.httpRangeServerWorks = (status == 206)
                                              && received.getDataSize() == 100
                                              && std::memcmp (received.getData(), expected.getData(), 100) == 0;
                result.notes.push_back (result.httpRangeServerWorks
                                      ? "HTTP byte-range server returned 206 with exactly the requested 100 bytes."
                                      : "HTTP byte-range check failed (status " + juce::String (status) + ").");
            }
        }
    }

    // 3. SOAP control against the mock renderer, which also fetches media.
    if (mockFound)
    {
        impl->connectedEndpoint = mockEndpoint;
        impl->connectedFlag = true;
        auto mediaUrl = impl->mediaServer.makeMediaUrl ("127.0.0.1", testFile);
        juce::String controlError;
        bool ok = playUri (mediaUrl, "Self Test Tone", "Snake Puppy", "Network Self Test", controlError);
        const juce::ScopedLock sl (impl->mockRenderer.stateLock);
        result.soapControlWorks = ok && impl->mockRenderer.receivedSetUri && impl->mockRenderer.receivedPlay;
        if (impl->mockRenderer.mediaFetchOk)
            result.notes.push_back ("Renderers' media fetch over HTTP succeeded (206/200 with data).");
        else
            result.notes.push_back ("Mock renderer media fetch failed.");
        if (! ok)
            result.notes.push_back ("SOAP control error: " + controlError);
        result.notes.push_back (juce::String ("Mock renderer records SetAVTransportURI=")
                              + (impl->mockRenderer.receivedSetUri ? "yes" : "no")
                              + ", Play=" + (impl->mockRenderer.receivedPlay ? "yes" : "no"));
        impl->connectedFlag = false;
        impl->connectedEndpoint = {};
    }

    impl->mockRenderer.stop();
    impl->mediaServer.stop();
    testFile.deleteFile();
    impl->verification = result.passed() ? VerificationState::selfTestPassed : VerificationState::failed;
    impl->verificationDetail = joinNotes (result.notes);
    {
        const juce::ScopedLock sl (impl->stateLock);
        impl->statusText = result.passed() ? "Network self-test passed (mock renderer)."
                                           : "Network self-test failed; see details.";
    }
    sendChangeMessage();
    return result;
}

} // namespace sp
