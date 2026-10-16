// Render templates: general-purpose formats, video sites and social apps
// (sizes, rates and limits from each platform's upload guidelines, 2026),
// messaging apps (sized to their file limits), devices old and new, disc
// and broadcast formats, editing intermediates, archival, animation, image
// sequences and audio.

#include "core/Exporter.h"

namespace {

// Fluent builder for one template.
struct T {
    ExportPreset e;
    T(const char* category, const char* name, const char* muxer, const char* ext, const char* video, const char* audio)
    {
        e.category = QString::fromUtf8(category);
        e.name = QString::fromUtf8(name);
        e.muxer = QString::fromLatin1(muxer);
        e.extension = QString::fromLatin1(ext);
        e.videoEncoder = QString::fromLatin1(video);
        e.audioEncoder = QString::fromLatin1(audio);
    }
    T& size(int w, int h = 0)
    {
        e.width = w;
        e.height = h;
        return *this;
    }
    T& rate(int num, int den = 1)
    {
        e.fps = FrameRate{num, den};
        return *this;
    }
    T& crf(int q)
    {
        e.quality = q;
        return *this;
    }
    T& q(int q) { return crf(q); }
    // Average video bitrate with a peak limit (VBR within a buffer).
    T& kbps(int k, double peak = 1.5)
    {
        e.videoBitrate = int64_t(k) * 1000;
        return opt(QStringLiteral("maxrate=%1k:bufsize=%2k").arg(int(k * peak)).arg(k * 2));
    }
    T& cbr(int k)
    {
        e.videoBitrate = int64_t(k) * 1000;
        return opt(QStringLiteral("minrate=%1k:maxrate=%1k:bufsize=%2k").arg(k).arg(k / 2));
    }
    T& preset(const char* p)
    {
        e.encoderPreset = QString::fromLatin1(p);
        return *this;
    }
    T& opt(const QString& o)
    {
        e.videoOptions = e.videoOptions.isEmpty() ? o : e.videoOptions + QLatin1Char(':') + o;
        return *this;
    }
    T& opt(const char* o) { return opt(QString::fromLatin1(o)); }
    T& pix(const char* p)
    {
        e.pixelFormat = QString::fromLatin1(p);
        return *this;
    }
    T& audio(int kbps, int rate = 48000, int channels = 2)
    {
        e.audioBitrate = int64_t(kbps) * 1000;
        e.sampleRate = rate;
        e.channels = channels;
        return *this;
    }
    T& note(const char* n)
    {
        e.notes = QString::fromUtf8(n);
        return *this;
    }
    T& maxLen(int seconds)
    {
        e.maxSeconds = seconds;
        return *this;
    }
    T& fitMB(double mb)
    {
        e.targetSizeMB = mb;
        return *this;
    }
    T& frame(Framing f)
    {
        e.framing = f;
        return *this;
    }
    T& mux(const char* o)
    {
        e.muxerOptions = QString::fromLatin1(o);
        return *this;
    }
    T& gop(double seconds)
    {
        e.gopSeconds = seconds;
        return *this;
    }
};

const char* kGeneral = "General";
const char* kYouTube = "YouTube";
const char* kSocial = "Social Media";
const char* kSites = "Video Sites & Streaming";
const char* kMessaging = "Messaging & Sharing";
const char* kDevices = "Devices";
const char* kRetro = "Retro & Legacy";
const char* kDisc = "Disc & Broadcast";
const char* kEditing = "Editing & Mastering";
const char* kArchive = "Archival & Lossless";
const char* kAnimation = "Animation";
const char* kStills = "Image Sequences";
const char* kAudio = "Audio Only";
const char* kGpu = "GPU Encoders";

// H.264 + AAC in MP4, the web's common denominator.
T h264(const char* cat, const char* name, int w, int h, int kbps)
{
    T t(cat, name, "mp4", "mp4", "libx264", "aac");
    t.size(w, h).preset("medium").opt("profile=high").audio(192);
    if (kbps > 0)
        t.kbps(kbps);
    else
        t.crf(20);
    return t;
}
T vertical(const char* cat, const char* name, int kbps = 12000)
{
    return h264(cat, name, 1080, 1920, kbps).frame(Framing::BlurFit);
}

} // namespace

const std::vector<ExportPreset>& exportPresets()
{
    static const std::vector<ExportPreset> presets = [] {
        std::vector<ExportPreset> p;
        auto add = [&](const T& t) { p.push_back(t.e); };

        // ---- General ----------------------------------------------------------
        add(h264(kGeneral, "MP4 - H.264 + AAC (most compatible)", 0, 0, 0).note("Plays almost everywhere. Project size and frame rate."));
        add(h264(kGeneral, "MP4 - H.264 high quality (CRF 16, slow)", 0, 0, 0).crf(16).preset("slow").audio(320)
                .note("Bigger files, very close to the original."));
        add(T(kGeneral, "MP4 - HEVC (H.265) + AAC", "mp4", "mp4", "libx265", "aac").crf(23).preset("medium").opt("tag=hvc1")
                .note("About half the size of H.264 at the same quality. Plays on recent phones, Macs, TVs and Windows with HEVC support."));
        add(T(kGeneral, "MP4 - HEVC 10-bit + AAC (smooth gradients)", "mp4", "mp4", "libx265", "aac").crf(22).preset("medium")
                .opt("tag=hvc1").pix("yuv420p10le").note("10-bit: no banding in skies and fades."));
        add(T(kGeneral, "MP4 - AV1 (SVT-AV1) + Opus", "mp4", "mp4", "libsvtav1", "libopus").crf(32).preset("8").audio(160)
                .note("Smallest files; plays in modern browsers, Android, Windows 11, recent TVs."));
        add(T(kGeneral, "MKV - AV1 10-bit + Opus", "matroska", "mkv", "libsvtav1", "libopus").crf(30).preset("6").pix("yuv420p10le")
                .audio(192));
        add(T(kGeneral, "WebM - VP9 + Opus", "webm", "webm", "libvpx-vp9", "libopus").crf(31).opt("row-mt=1:deadline=good:cpu-used=2")
                .audio(160).note("Open format for the web; every modern browser plays it."));
        add(T(kGeneral, "WebM - AV1 + Opus", "webm", "webm", "libsvtav1", "libopus").crf(34).preset("8").audio(160));
        add(T(kGeneral, "WebM - VP8 + Vorbis (older browsers)", "webm", "webm", "libvpx", "libvorbis").kbps(4000).opt("deadline=good:cpu-used=2")
                .audio(160));
        add(T(kGeneral, "MKV - H.264 + FLAC (lossless audio)", "matroska", "mkv", "libx264", "flac").crf(18).preset("slow"));
        add(T(kGeneral, "MKV - H.266 / VVC + Opus (experimental)", "matroska", "mkv", "libvvenc", "libopus").crf(30).preset("fast").audio(160)
                .note("The newest standard: smaller than HEVC and AV1, but few players support it yet."));
        add(T(kGeneral, "OGV - Theora + Vorbis (Wikimedia Commons)", "ogg", "ogv", "libtheora", "libvorbis").q(7).audio(160)
                .note("Free formats accepted by Wikipedia / Wikimedia Commons."));

        // ---- YouTube (YouTube's recommended upload settings) ------------------------
        add(h264(kYouTube, "YouTube 720p", 1280, 720, 5000).audio(384).gop(0.5).opt("bf=2").note("YouTube recommends 5 Mb/s for 720p."));
        add(h264(kYouTube, "YouTube 720p 60 fps", 1280, 720, 7500).rate(60).audio(384).gop(0.5).opt("bf=2"));
        add(h264(kYouTube, "YouTube 1080p", 1920, 1080, 8000).audio(384).gop(0.5).opt("bf=2").note("8 Mb/s, AAC 384 kb/s, as YouTube recommends."));
        add(h264(kYouTube, "YouTube 1080p 60 fps", 1920, 1080, 12000).rate(60).audio(384).gop(0.5).opt("bf=2"));
        add(h264(kYouTube, "YouTube 1440p (2K)", 2560, 1440, 16000).audio(384).gop(0.5).opt("bf=2"));
        add(h264(kYouTube, "YouTube 1440p 60 fps", 2560, 1440, 24000).rate(60).audio(384).gop(0.5).opt("bf=2"));
        add(h264(kYouTube, "YouTube 4K", 3840, 2160, 45000).audio(384).gop(0.5).opt("bf=2").note("35-45 Mb/s recommended for 4K."));
        add(h264(kYouTube, "YouTube 4K 60 fps", 3840, 2160, 68000).rate(60).audio(384).gop(0.5).opt("bf=2"));
        add(T(kYouTube, "YouTube 4K - HEVC (smaller upload)", "mp4", "mp4", "libx265", "aac").size(3840, 2160).crf(20).preset("medium")
                .opt("tag=hvc1").audio(384).note("Same quality as the H.264 4K template in about half the size: quicker uploads."));
        add(T(kYouTube, "YouTube 8K - AV1", "mp4", "mp4", "libsvtav1", "aac").size(7680, 4320).crf(28).preset("8").audio(384)
                .note("YouTube shows 8K for AV1 uploads; slow to render."));
        add(vertical(kYouTube, "YouTube Shorts (1080x1920)", 10000).audio(384).maxLen(180).note("Vertical 9:16, up to 3 minutes."));
        add(h264(kYouTube, "Audio-Only YouTube (album art, 5 fps)", 1920, 1080, 0).rate(5).crf(18).opt("tune=stillimage").gop(2).audio(384)
                .note("Music over a still picture: 5 fps keeps the picture to a few MB per hour; AAC 384 kb/s, as YouTube recommends."));

        // ---- Social media -----------------------------------------------------------
        add(vertical(kSocial, "TikTok (1080x1920)").maxLen(600).note("9:16, up to 10 minutes; up to 1 GB through the web uploader."));
        add(vertical(kSocial, "TikTok 60 fps (1080x1920)", 16000).rate(60).maxLen(600));
        add(vertical(kSocial, "Instagram Reels (1080x1920)").maxLen(180).note("9:16, 3 s to 3 minutes, up to 4 GB."));
        add(h264(kSocial, "Instagram Feed - square (1080x1080)", 1080, 1080, 8000).frame(Framing::BlurFit).maxLen(3600));
        add(h264(kSocial, "Instagram Feed - portrait 4:5 (1080x1350)", 1080, 1350, 9000).frame(Framing::BlurFit)
                .note("4:5 takes the most room in the feed."));
        add(vertical(kSocial, "Instagram Stories (1080x1920)").maxLen(60).note("Each story shows up to 60 seconds."));
        add(h264(kSocial, "Facebook Feed 1080p", 1920, 1080, 10000).note("H.264 + AAC 48 kHz stereo, up to 25 Mb/s."));
        add(vertical(kSocial, "Facebook Reels (1080x1920)").maxLen(90).note("Up to 90 seconds."));
        add(vertical(kSocial, "Facebook Stories (1080x1920)").maxLen(20).note("20 seconds per story card."));
        add(h264(kSocial, "X (Twitter) 720p", 1280, 720, 5000).audio(128).maxLen(140)
                .note("2:20 and 512 MB for standard accounts (Premium: longer). 1280x720 is X's recommendation."));
        add(h264(kSocial, "X (Twitter) 1080p", 1920, 1080, 8000).audio(128).maxLen(140).note("Largest size X accepts is 1920x1200."));
        add(h264(kSocial, "X (Twitter) vertical (720x1280)", 720, 1280, 5000).audio(128).frame(Framing::BlurFit).maxLen(140));
        add(vertical(kSocial, "Threads (1080x1920)").maxLen(300).note("Up to 5 minutes and 1 GB."));
        add(h264(kSocial, "Bluesky 1080p", 1920, 1080, 3500).maxLen(600).note("Up to 10 minutes and 300 MB: 3.5 Mb/s keeps 10 minutes under the limit."));
        add(h264(kSocial, "Bluesky vertical (1080x1920)", 1080, 1920, 3500).frame(Framing::BlurFit).maxLen(600));
        add(h264(kSocial, "LinkedIn 1080p", 1920, 1080, 8000).maxLen(900).note("3 s to 15 minutes (10 on mobile), up to 5 GB."));
        add(h264(kSocial, "LinkedIn square (1080x1080)", 1080, 1080, 8000).frame(Framing::BlurFit).maxLen(900));
        add(vertical(kSocial, "Snapchat Spotlight (1080x1920, 32 MB)").fitMB(31).maxLen(180)
                .note("Spotlight takes up to 3 minutes and 32 MB: the render is kept under the limit."));
        add(h264(kSocial, "Pinterest video pin (1000x1500, 2:3)", 1000, 1500, 8000).frame(Framing::BlurFit).maxLen(900)
                .note("2:3, up to 15 minutes and 2 GB."));
        add(h264(kSocial, "Reddit 1080p", 1920, 1080, 8000).maxLen(900).note("Up to 15 minutes and 1 GB."));
        add(h264(kSocial, "Tumblr 720p", 1280, 720, 5000));

        // ---- Video sites & streaming ---------------------------------------------------
        add(h264(kSites, "Vimeo 720p", 1280, 720, 8000).audio(320).note("Vimeo: 5-10 Mb/s for 720p, H.264 High profile."));
        add(h264(kSites, "Vimeo 1080p", 1920, 1080, 18000).audio(320).note("Vimeo: 10-20 Mb/s for 1080p."));
        add(T(kSites, "Vimeo 4K - HEVC", "mp4", "mp4", "libx265", "aac").size(3840, 2160).kbps(45000).preset("medium").opt("tag=hvc1").audio(320)
                .note("Vimeo recommends HEVC for 4K, 30-60 Mb/s."));
        add(h264(kSites, "Twitch video upload 1080p 60 fps", 1920, 1080, 10000).rate(60).audio(160).gop(2));
        add(h264(kSites, "Dailymotion 1080p", 1920, 1080, 8000));
        add(h264(kSites, "Rumble 1080p", 1920, 1080, 8000));
        add(h264(kSites, "Steam store trailer (1080p)", 1920, 1080, 15000).rate(30).audio(256)
                .maxLen(600).note("MP4 H.264 1920x1080, under 10 minutes and 2 GB."));
        add(h264(kSites, "Kickstarter / Patreon 1080p", 1920, 1080, 10000).audio(256));
        add(h264(kSites, "Spotify video podcast (1080p)", 1920, 1080, 8000).audio(320));
        add(T(kSites, "HLS stream (web servers, .m3u8 + segments)", "hls", "m3u8", "libx264", "aac").kbps(6000).preset("medium")
                .opt("profile=high").gop(2).mux("hls_time=6:hls_playlist_type=vod")
                .note("Apple HTTP Live Streaming: a playlist plus 6-second .ts segments, for hosting on any web server."));
        add(T(kSites, "MPEG-DASH stream (.mpd + segments)", "dash", "mpd", "libx264", "aac").kbps(6000).preset("medium")
                .opt("profile=high").gop(2).mux("seg_duration=4"));

        // ---- Messaging & sharing (sized to the apps' limits) ---------------------------------
        add(h264(kMessaging, "Discord (free, 20 MB)", 1280, 720, 0).audio(96).fitMB(19.5)
                .note("Discord's free limit is 20 MB per file: the render is kept under it (long renders get a lower bitrate)."));
        add(h264(kMessaging, "Discord Nitro Basic (50 MB)", 1920, 1080, 0).audio(128).fitMB(49));
        add(h264(kMessaging, "Discord Nitro (500 MB)", 1920, 1080, 0).audio(160).fitMB(490));
        add(h264(kMessaging, "WhatsApp (16 MB)", 1280, 720, 0).audio(96).fitMB(15.5)
                .note("Videos sent as media are limited to 16 MB: the render is kept under it."));
        add(h264(kMessaging, "Telegram 1080p", 1920, 1080, 6000).note("Up to 2 GB (4 GB with Premium)."));
        add(h264(kMessaging, "Signal (95 MB)", 1920, 1080, 0).audio(128).fitMB(95));
        add(h264(kMessaging, "Email attachment (24 MB)", 1280, 720, 0).audio(96).fitMB(24)
                .note("Gmail and Outlook accept attachments up to 25 MB."));
        add(h264(kMessaging, "Small preview (480p, 1.5 Mb/s)", 854, 480, 1500).audio(96));

        // ---- Devices ---------------------------------------------------------------------
        add(T(kDevices, "iPhone / iPad - HEVC 1080p", "mp4", "m4v", "libx265", "aac").size(1920, 1080).crf(22).preset("medium").opt("tag=hvc1")
                .audio(192));
        add(T(kDevices, "iPhone / iPad - HEVC 4K", "mp4", "m4v", "libx265", "aac").size(3840, 2160).crf(22).preset("medium").opt("tag=hvc1")
                .audio(192));
        add(T(kDevices, "Apple TV 4K - HEVC 10-bit", "mp4", "m4v", "libx265", "aac").size(3840, 2160).crf(20).preset("medium").opt("tag=hvc1")
                .pix("yuv420p10le").audio(256));
        add(T(kDevices, "Mac / QuickTime - H.264 MOV", "mov", "mov", "libx264", "aac").crf(18).preset("medium").opt("profile=high").audio(256));
        add(h264(kDevices, "Android phone 1080p", 1920, 1080, 8000));
        add(h264(kDevices, "PlayStation 5 (USB, up to 4K)", 0, 0, 0).crf(18).opt("level=5.2")
                .note("PS5 Media Gallery: MP4 H.264 High up to level 5.2 (4K) with AAC, from an exFAT USB drive. No HEVC."));
        add(h264(kDevices, "PlayStation 4 (USB, 1080p)", 1920, 1080, 12000).opt("level=4.2"));
        add(h264(kDevices, "Xbox Series X|S / Xbox One", 0, 0, 0).crf(18).opt("level=5.1"));
        add(h264(kDevices, "Steam Deck (1280x800)", 1280, 800, 5000).note("16:10 screen."));
        add(h264(kDevices, "Raspberry Pi (1080p30, hardware decoding)", 1920, 1080, 8000).rate(30).opt("level=4.1"));
        add(T(kDevices, "Smart TV / media player - MKV H.264 + AC-3", "matroska", "mkv", "libx264", "ac3").crf(18).preset("medium")
                .opt("profile=high:level=4.1").audio(448));
        add(h264(kDevices, "Chromecast / Roku / Fire TV 1080p", 1920, 1080, 10000).opt("level=4.1"));
        add(h264(kDevices, "Tablet 1280x800", 1280, 800, 4000));
        add(T(kDevices, "Digital photo frame - MJPEG AVI 800x480", "avi", "avi", "mjpeg", "pcm_s16le").size(800, 480).q(3).rate(24)
                .frame(Framing::Fill).audio(0, 44100));

        // ---- Retro & legacy -----------------------------------------------------------------
        add(T(kRetro, "Sony PSP (480x272)", "psp", "mp4", "libx264", "aac").size(480, 272).kbps(768).rate(30000, 1001)
                .opt("profile=main:level=3.0:refs=3:bf=0").audio(128).note("Copy into the VIDEO folder on the Memory Stick."));
        add(T(kRetro, "Sony PS Vita (960x544)", "mp4", "mp4", "libx264", "aac").size(960, 544).kbps(2500).opt("profile=main:level=4.0").audio(160));
        add(T(kRetro, "PlayStation 3 (MP4 1080p)", "mp4", "mp4", "libx264", "aac").size(1920, 1080).kbps(12000).opt("profile=high:level=4.1")
                .audio(192));
        add(T(kRetro, "Xbox 360 (MP4 720p)", "mp4", "mp4", "libx264", "aac").size(1280, 720).kbps(5000).opt("profile=main:level=4.1").audio(160));
        add(T(kRetro, "iPod Classic / iPod video (640x480)", "ipod", "m4v", "libx264", "aac").size(640, 480).kbps(1500)
                .opt("profile=baseline:level=3.0").frame(Framing::Fit).audio(128, 44100));
        add(T(kRetro, "iPod nano (320x240)", "ipod", "m4v", "libx264", "aac").size(320, 240).kbps(700).opt("profile=baseline:level=1.3")
                .frame(Framing::Fit).audio(128, 44100));
        add(T(kRetro, "Nintendo 3DS (MJPEG AVI 480x240, 20 fps)", "avi", "avi", "mjpeg", "adpcm_ima_wav").size(480, 240).rate(20).q(3)
                .audio(0, 16000, 1).note("Plays in the 3DS Camera / SD card video player."));
        add(T(kRetro, "Nintendo Wii Photo Channel (MJPEG AVI 640x480)", "avi", "avi", "mjpeg", "pcm_s16le").size(640, 480).rate(30).q(4)
                .frame(Framing::Fit).audio(0, 32000).note("Motion-JPEG AVI from an SD card, as played by the Photo Channel 1.1."));
        add(T(kRetro, "Old mobile phone - 3GP H.263 176x144 + AMR", "3gp", "3gp", "h263", "libopencore_amrnb").size(176, 144).rate(15)
                .kbps(128).frame(Framing::Fit).audio(12, 8000, 1));
        add(T(kRetro, "Feature phone - 3GP MPEG-4 320x240 + AAC", "3gp", "3gp", "mpeg4", "aac").size(320, 240).rate(15).kbps(384)
                .frame(Framing::Fit).audio(64, 22050, 1));
        add(T(kRetro, "DivX / Xvid AVI + MP3 (DVD players with DivX)", "avi", "avi", "libxvid", "libmp3lame").size(720).kbps(2000)
                .opt("tag=XVID").audio(192, 48000));
        add(T(kRetro, "Windows Media (WMV + WMA)", "asf", "wmv", "wmv2", "wmav2").kbps(4000).audio(192, 44100)
                .note("For old Windows PCs and Windows Media Player."));
        add(T(kRetro, "Flash Video (FLV, Sorenson + MP3)", "flv", "flv", "flv", "libmp3lame").size(640).kbps(1200).audio(128, 44100)
                .note("The 2000s web video format."));
        add(T(kRetro, "CD-ROM era - Cinepak MOV 320x240 15 fps", "mov", "mov", "cinepak", "pcm_u8").size(320, 240).rate(15)
                .frame(Framing::Fit).audio(0, 22050, 1).note("Like 1990s multimedia CD-ROMs and early QuickTime."));
        add(T(kRetro, "id Software RoQ (Quake III cutscenes)", "roq", "roq", "roqvideo", "roq_dpcm").size(512, 256).rate(30)
                .frame(Framing::Fit).audio(0, 22050, 1).note("Quake III Arena / id Tech 3 cinematic format."));

        // ---- Disc & broadcast ---------------------------------------------------------------
        add(T(kDisc, "DVD NTSC - MPEG-2 720x480 29.97 + AC-3", "dvd", "vob", "mpeg2video", "ac3").size(720, 480).rate(30000, 1001)
                .kbps(6000, 1.5).opt("maxrate=9000k:bufsize=1835k").audio(192).note("For DVD authoring tools."));
        add(T(kDisc, "DVD PAL - MPEG-2 720x576 25 + AC-3", "dvd", "vob", "mpeg2video", "ac3").size(720, 576).rate(25).kbps(6000, 1.5)
                .opt("maxrate=9000k:bufsize=1835k").audio(192));
        add(T(kDisc, "Video CD NTSC - MPEG-1 352x240", "vcd", "mpg", "mpeg1video", "mp2").size(352, 240).rate(30000, 1001).cbr(1150)
                .audio(224, 44100).note("White Book VCD: 1150 kb/s video, 224 kb/s MP2 audio."));
        add(T(kDisc, "Video CD PAL - MPEG-1 352x288", "vcd", "mpg", "mpeg1video", "mp2").size(352, 288).rate(25).cbr(1150).audio(224, 44100));
        add(T(kDisc, "Super VCD NTSC - MPEG-2 480x480", "svcd", "mpg", "mpeg2video", "mp2").size(480, 480).rate(30000, 1001).kbps(2200, 1.1)
                .audio(224, 44100));
        add(T(kDisc, "Super VCD PAL - MPEG-2 480x576", "svcd", "mpg", "mpeg2video", "mp2").size(480, 576).rate(25).kbps(2200, 1.1)
                .audio(224, 44100));
        add(T(kDisc, "Blu-ray - H.264 1080p (M2TS)", "mpegts", "m2ts", "libx264", "ac3").size(1920, 1080).kbps(25000, 1.6).preset("slow")
                .opt("profile=high:level=4.1:x264-params='bluray-compat=1:open-gop=0:slices=4:nal-hrd=vbr'").gop(1).audio(640)
                .mux("mpegts_m2ts_mode=1").note("Blu-ray compatible H.264 for authoring tools; use 23.976, 24, 25 or 29.97 fps."));
        add(T(kDisc, "AVCHD - H.264 1080p (MTS)", "mpegts", "mts", "libx264", "ac3").size(1920, 1080).kbps(17000, 1.4)
                .opt("profile=high:level=4.1").audio(256).mux("mpegts_m2ts_mode=1"));
        add(T(kDisc, "MiniDV NTSC (DV 720x480)", "dv", "dv", "dvvideo", "pcm_s16le").size(720, 480).rate(30000, 1001).pix("yuv411p").audio(0, 48000)
                .frame(Framing::Fill));
        add(T(kDisc, "MiniDV PAL (DV 720x576)", "dv", "dv", "dvvideo", "pcm_s16le").size(720, 576).rate(25).audio(0, 48000).frame(Framing::Fill));
        add(T(kDisc, "XDCAM HD422 style - MPEG-2 4:2:2 50 Mb/s (MXF)", "mxf", "mxf", "mpeg2video", "pcm_s24le").size(1920, 1080)
                .cbr(50000).pix("yuv422p").opt("non_linear_quant=1:intra_vlc=1:qmax=3:dc=10").gop(0.5).audio(0, 48000)
                .note("Broadcast delivery format (progressive)."));
        add(T(kDisc, "Broadcast MPEG-TS - H.264 CBR 15 Mb/s", "mpegts", "ts", "libx264", "mp2").size(1920, 1080).cbr(15000)
                .opt("profile=high:level=4.0:x264-params='nal-hrd=cbr'").gop(0.5).audio(256));

        // ---- Editing & mastering ----------------------------------------------------------------
        const struct {
            const char* name;
            int profile;
            const char* pix;
        } prores[] = {{"MOV - ProRes 422 Proxy", 0, "yuv422p10le"},
                      {"MOV - ProRes 422 LT", 1, "yuv422p10le"},
                      {"MOV - ProRes 422", 2, "yuv422p10le"},
                      {"MOV - ProRes 422 HQ + PCM (editing master)", 3, "yuv422p10le"},
                      {"MOV - ProRes 4444", 4, "yuv444p10le"},
                      {"MOV - ProRes 4444 XQ", 5, "yuv444p10le"}};
        for (const auto& r : prores)
            add(T(kEditing, r.name, "mov", "mov", "prores_ks", "pcm_s16le").opt(QStringLiteral("profile=%1").arg(r.profile)).pix(r.pix)
                    .audio(0, 48000).note("Apple ProRes: edits smoothly in Final Cut, Premiere, Resolve and After Effects."));
        const struct {
            const char* name;
            const char* profile;
            const char* pix;
        } dnx[] = {{"MOV - DNxHR LB (offline)", "dnxhr_lb", "yuv422p"},     {"MOV - DNxHR SQ", "dnxhr_sq", "yuv422p"},
                   {"MOV - DNxHR HQ", "dnxhr_hq", "yuv422p"},               {"MOV - DNxHR HQX 10-bit", "dnxhr_hqx", "yuv422p10le"},
                   {"MOV - DNxHR 444 10-bit", "dnxhr_444", "yuv444p10le"}};
        for (const auto& r : dnx)
            add(T(kEditing, r.name, "mov", "mov", "dnxhd", "pcm_s16le").opt(QStringLiteral("profile=%1").arg(QString::fromLatin1(r.profile)))
                    .pix(r.pix).audio(0, 48000).note("Avid DNxHR: any resolution; edits smoothly in Avid, Premiere and Resolve."));
        add(T(kEditing, "MXF OP1a - DNxHR HQ (Avid)", "mxf", "mxf", "dnxhd", "pcm_s24le").opt("profile=dnxhr_hq").pix("yuv422p")
                .audio(0, 48000));
        add(T(kEditing, "MOV - GoPro CineForm 10-bit", "mov", "mov", "cfhd", "pcm_s16le").pix("yuv422p10le").audio(0, 48000));
        add(T(kEditing, "MOV - HAP (VJ software: Resolume, VDMX)", "mov", "mov", "hap", "").opt("format=hap")
                .note("GPU-decoded DXT textures: many layers play at once in VJ software (no audio)."));
        add(T(kEditing, "MOV - HAP Q (higher quality VJ clips)", "mov", "mov", "hap", "").opt("format=hap_q"));
        add(T(kEditing, "MP4 - H.264 all-intra (fast scrubbing)", "mp4", "mp4", "libx264", "aac").crf(12).preset("fast").gop(0.001)
                .opt("profile=high422").pix("yuv422p").audio(320).note("Every frame is a keyframe: scrubs and cuts instantly in any editor."));
        add(T(kEditing, "MOV - Uncompressed 10-bit 4:2:2 (v210)", "mov", "mov", "v210", "pcm_s24le").audio(0, 48000)
                .note("Huge files; for systems that need uncompressed video."));
        add(T(kEditing, "MOV - QuickTime Animation (RLE)", "mov", "mov", "qtrle", "pcm_s16le").audio(0, 48000));
        add(T(kEditing, "MOV - PNG frames", "mov", "mov", "png", "pcm_s16le").audio(0, 48000));
        add(T(kEditing, "AVI - Motion JPEG (high quality)", "avi", "avi", "mjpeg", "pcm_s16le").q(2).audio(0, 48000));

        // ---- Archival & lossless ------------------------------------------------------------------
        add(T(kArchive, "MKV - FFV1 lossless RGB + FLAC (archive master)", "matroska", "mkv", "ffv1", "flac").opt("level=3:slicecrc=1:slices=16")
                .pix("bgr0").gop(0.001).note("Bit-exact, with checksums per slice: the standard for film and video archives."));
        add(T(kArchive, "MKV - H.264 lossless RGB + FLAC", "matroska", "mkv", "libx264rgb", "flac").crf(0).preset("veryfast"));
        add(T(kArchive, "AVI - UT Video lossless + PCM", "avi", "avi", "utvideo", "pcm_s16le").pix("gbrp").audio(0, 48000));
        add(T(kArchive, "AVI - HuffYUV lossless + PCM", "avi", "avi", "huffyuv", "pcm_s16le").pix("bgra").audio(0, 48000));
        add(T(kArchive, "NUT - FFV1 lossless 4:2:0", "nut", "nut", "ffv1", "flac").opt("level=3").pix("yuv420p"));

        // ---- Animation --------------------------------------------------------------------------------
        add(T(kAnimation, "GIF - animated, 480 wide, 15 fps", "gif", "gif", "gif", "").size(480).rate(15));
        add(T(kAnimation, "GIF - small for chat, 320 wide, 10 fps", "gif", "gif", "gif", "").size(320).rate(10));
        add(T(kAnimation, "GIF - large, 800 wide, 20 fps", "gif", "gif", "gif", "").size(800).rate(20));
        add(T(kAnimation, "Animated PNG (APNG), 480 wide", "apng", "png", "apng", "").size(480).rate(20).mux("plays=0")
                .note("Full colour with no dithering; plays in every modern browser."));
        add(T(kAnimation, "Animated WebP, 480 wide", "webp", "webp", "libwebp_anim", "").size(480).rate(20).opt("quality=80").mux("loop=0")
                .note("Much smaller than GIF, full colour."));

        // ---- Image sequences (a numbered file per frame) ------------------------------------------------
        const struct {
            const char* name;
            const char* ext;
            const char* encoder;
            const char* pix;
            const char* options;
            int quality;
        } stills[] = {{"PNG sequence (8-bit)", "png", "png", "rgb24", "", -1},
                      {"PNG sequence (16-bit)", "png", "png", "rgb48be", "", -1},
                      {"JPEG sequence", "jpg", "mjpeg", "yuvj444p", "", 2},
                      {"TIFF sequence (LZW)", "tif", "tiff", "rgb24", "compression_algo=lzw", -1},
                      {"DPX sequence (10-bit, film / VFX)", "dpx", "dpx", "gbrp10le", "", -1},
                      {"OpenEXR sequence (half float, VFX)", "exr", "exr", "gbrpf32le", "compression=3:format=1", -1},
                      {"Targa (TGA) sequence", "tga", "targa", "bgr24", "", -1},
                      {"BMP sequence", "bmp", "bmp", "bgr24", "", -1},
                      {"WebP sequence", "webp", "libwebp", "", "quality=90", -1},
                      {"JPEG 2000 sequence", "jp2", "libopenjpeg", "rgb24", "", -1},
                      {"JPEG XL sequence", "jxl", "libjxl", "rgb24", "distance=1", -1},
                      {"QOI sequence", "qoi", "qoi", "rgb24", "", -1}};
        for (const auto& st : stills) {
            T t(kStills, st.name, "image2", st.ext, st.encoder, "");
            if (*st.pix)
                t.pix(st.pix);
            if (*st.options)
                t.opt(st.options);
            if (st.quality >= 0)
                t.q(st.quality);
            add(t.note("Writes name_00001.ext, name_00002.ext, ... (no audio)."));
        }

        // ---- Audio only ---------------------------------------------------------------------------------------
        auto audioOnly = [&](const char* name, const char* mux, const char* ext, const char* enc, int kbps, int rate = 48000, int ch = 2) {
            return T(kAudio, name, mux, ext, "", enc).audio(kbps, rate, ch);
        };
        add(audioOnly("WAV - 16-bit PCM", "wav", "wav", "pcm_s16le", 0));
        add(audioOnly("WAV - 24-bit PCM (studio)", "wav", "wav", "pcm_s24le", 0));
        add(audioOnly("WAV - 32-bit float", "wav", "wav", "pcm_f32le", 0));
        add(audioOnly("AIFF - 16-bit (Mac)", "aiff", "aiff", "pcm_s16be", 0, 44100));
        add(audioOnly("FLAC - lossless", "flac", "flac", "flac", 0));
        add(audioOnly("ALAC - Apple Lossless (M4A)", "ipod", "m4a", "alac", 0, 44100));
        add(audioOnly("MP3 - 320 kb/s", "mp3", "mp3", "libmp3lame", 320, 44100));
        add(audioOnly("MP3 - 192 kb/s", "mp3", "mp3", "libmp3lame", 192, 44100));
        add(audioOnly("MP3 - podcast / voice (96 kb/s mono)", "mp3", "mp3", "libmp3lame", 96, 44100, 1));
        add(audioOnly("AAC - 256 kb/s (M4A, Apple Music)", "ipod", "m4a", "aac", 256, 44100));
        add(audioOnly("AAC - podcast (128 kb/s M4A)", "ipod", "m4a", "aac", 128, 44100));
        add(audioOnly("iPhone ringtone (M4R, 30 s)", "ipod", "m4r", "aac", 192, 44100).maxLen(30)
                .note("Ringtones can be up to 30 seconds (40 s for alert tones)."));
        add(audioOnly("Opus - 160 kb/s", "opus", "opus", "libopus", 160));
        add(audioOnly("Ogg Vorbis - 192 kb/s (games)", "ogg", "ogg", "libvorbis", 192, 44100));
        add(audioOnly("AC-3 / Dolby Digital - 448 kb/s", "ac3", "ac3", "ac3", 448));
        add(audioOnly("WMA - 192 kb/s", "asf", "wma", "wmav2", 192, 44100));
        add(audioOnly("WavPack - lossless", "wv", "wv", "wavpack", 0));
        add(audioOnly("AMR-NB - voice (8 kHz mono)", "amr", "amr", "libopencore_amrnb", 12, 8000, 1));

        // ---- GPU encoders (listed when this PC's GPU supports them) ----------------------------------------
        add(T(kGpu, "MP4 - H.264, NVIDIA GPU (NVENC) - fast", "mp4", "mp4", "h264_nvenc", "aac").crf(21).preset("p5").opt("rc=vbr:tune=hq"));
        add(T(kGpu, "MP4 - HEVC, NVIDIA GPU (NVENC) - fast", "mp4", "mp4", "hevc_nvenc", "aac").crf(24).preset("p5").opt("rc=vbr:tune=hq"));
        add(T(kGpu, "MP4 - AV1, NVIDIA GPU (NVENC) - fast", "mp4", "mp4", "av1_nvenc", "aac").crf(30).preset("p5").opt("rc=vbr:tune=hq"));
        add(T(kGpu, "MP4 - H.264, AMD GPU (AMF) - fast", "mp4", "mp4", "h264_amf", "aac").kbps(16000));
        add(T(kGpu, "MP4 - HEVC, AMD GPU (AMF) - fast", "mp4", "mp4", "hevc_amf", "aac").kbps(16000));
        add(T(kGpu, "MP4 - AV1, AMD GPU (AMF) - fast", "mp4", "mp4", "av1_amf", "aac").kbps(12000));
        add(T(kGpu, "MP4 - H.264, Intel GPU (Quick Sync) - fast", "mp4", "mp4", "h264_qsv", "aac").kbps(16000));
        add(T(kGpu, "MP4 - HEVC, Intel GPU (Quick Sync) - fast", "mp4", "mp4", "hevc_qsv", "aac").kbps(16000));
        add(T(kGpu, "MP4 - AV1, Intel GPU (Quick Sync) - fast", "mp4", "mp4", "av1_qsv", "aac").kbps(12000));
        return p;
    }();
    return presets;
}
