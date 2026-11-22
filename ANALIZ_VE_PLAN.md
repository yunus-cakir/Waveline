# Waveline — Analiz ve İmplementasyon Planı

> **Waveline Clip** = hafif Moments karşılığı · **Waveline Mix** = hafif Sonar karşılığı

> Tarih: 22.11.2026 · İncelenen kurulum: SteelSeries GG **v124.0.0**, Moments gsdk **2.5.28.0**, Sonar APO sürücüsü **3.10.0.0**
> Sistem: RTX 5070 (sürücü 621.01, 12 GB) · Ryzen 5 7500F · 32 GB RAM · 1920×1080 @ 143 Hz · ek olarak Parsec Virtual Display

---

## 0. Alınan kararlar (22.11.2026)

| Konu | Karar |
|---|---|
| İsim | Uygulama: **Waveline** · kayıt: **Waveline Clip** · mikser: **Waveline Mix** · Proje klasörü: `C:\Library\Projects\Visual Studio\Waveline` |
| teknoloji | c++20 çekirdek (kayıt motoru, mikser) + C# .NET 8 arayüz (ayrı süreç) |
| Dağıtım | Açık kaynak. Sanal ses sürücüsü **paketlenmez**; kullanıcı hazır imzalı bir sanal kablo sürücüsünü kendi rızasıyla kurar, uygulama onu algılar. Sürücü katmanı arayüz arkasında, ileride kendi imzalı sürücümüze geçilebilir |
| Codec / kalite | HEVC, 1080p60, ~15 Mbps varsayılan (H.264 uyumluluk seçeneği ayarlarda) |
| Ses izleri | 1 Mix · 2 Game · 3 Chat · 4 Media · 5 Mic |
| Geriye dönük süre | 30 sn – 10 dk, tamamen RAM'de (disk halkası yok) |
| Manuel kayıt | Buffer ile aynı anda, aynı NVENC oturumundan |
| Bildirim | Ses efekti + Windows bildirimi, overlay yok |
| Algılama | Tam ekran / borderless her uygulama; tarayıcı ve medya oynatıcılar varsayılan kara listede, liste düzenlenebilir |
| Masaüstü modu | Ayarlardan sabit seçilen monitör; Parsec sanal ekranı listede gizlenir |
| Kütüphane | İlk sürümde yok, klipler sadece `E:\Records`'a yazılır |
| Mic izi | İşlenmiş (gain / gate / gürültü bastırma sonrası) mikrofon |
| Sonar ekstra | Gain + noise gate + **RNNoise gürültü bastırma**; EQ, limiter, Aux ilk sürümde yok |
| Stream Mix | OBS yayın/kayıt için ses kaynağı |
| Hoparlör seçimi | Yol 1: Personal Mix seçilen cihaza otomatik geçer, sanal cihaz adı ona göre değişir |
| Yetki | Kurulumda yönetici izni + küçük arka plan servisi kabul |
| Geçiş dönemi | GG Moments kapatılır, Sonar açık kalır |
| OBS | Mod B (köprü), sonraki planda; yalnızca OBS'de yayın/kayıt başladığında devreye girer |

---

## 1. Mevcut yapının analizi

### 1.1 Genel kaynak tüketimi (inceleme anındaki ölçüm)

| Süreç | RAM (WS) | Not |
|---|---|---|
| SteelSeriesSonar.exe (.NET 8 + ASP.NET) | ~183 MB | Toplam ~5900 sn CPU harcamış, en ağır parça |
| SteelSeriesGGClient.exe (Electron, 8 süreç) | ~390 MB | Arayüz, sürekli çalışıyor |
| SteelSeriesEngine / GG / GGEZ / Prism | ~200 MB | Cihaz yönetimi, Moments/Sonar için gerekli değil |
| SteelSeriesMoments.exe (Go) | ~19 MB | Kayıt yokken boşta |
| **Toplam** | **~790 MB** | |

Hedef: Moments çekirdeği boştayken < 30 MB, kayıt sırasında < 80 MB + tampon; Sonar mikseri < 40 MB, CPU < %1.

### 1.2 Moments nasıl çalışıyor

**Bileşenler** (`C:\Program Files\SteelSeries\GG\apps\moments`)

| Dosya | Görev |
|---|---|
| `SteelSeriesMoments.exe` | Go ile yazılmış orkestratör: ayarlar, oyun listesi, klip kütüphanesi (SQLite + goose migration), kısayollar |
| `gsdk.dll` | C++ kayıt motoru: görüntü yakalama, ses mikseri, encoder, muxer |
| `gsdk-dxgidup-gpupref.exe` | DXGI Desktop Duplication'ı doğru GPU'da çalıştıran yardımcı (hibrit GPU sistemleri için) |
| `gsdk-ffmpeg.dll`, `ffmpeg.exe` | Encoder/mux katmanı ve kırpma/dışa aktarma |
| `SteelSeriesCaptureSvc.exe` | Yakalama yardımcı servisi |

**Görüntü hattı** (binary string'leri ve loglardan doğrulandı)
- Pencere yakalama: **Windows.Graphics.Capture (WGC)** → `Win11WindowCapture` (187 klip `window` modunda)
- Monitör yakalama: **DXGI `DuplicateOutput1`** (20 klip `monitor` modunda)
- Exclusive fullscreen'de: log'da `Fullscreen exclusive mode detected force stop` → **exclusive fullscreen desteklenmiyor, kayıt duruyor**
- Encoder: FFmpeg üzerinden **`h264_nvenc`**, D3D11/NV12 donanım bağlamı, log'da `copy with gpu, fast path (single copy)` → GPU'dan CPU'ya kopya yok (zero-copy'e yakın)
- Çıktı: H.264 **Main** profil, 1080p60, ~19 Mbps, MP4

> ⚠️ **Önemli bulgu:** Moments NVIDIA ShadowPlay/Instant Replay altyapısını **kullanmıyor**; NVENC'i FFmpeg üzerinden kullanıyor. ShadowPlay'in (NVIDIA App) dışarıya açık bir API'si yok, NvFBC ise GeForce kartlarda Windows'ta kullanıma kapalı. Yani "NVIDIA kayıt altyapısı" için doğru yol: **WGC/DXGI ile yakalama + NVENC SDK'yı doğrudan kullanmak** (OBS'nin yaptığı gibi). Biz FFmpeg katmanını da atlayıp NVENC SDK'yı doğrudan çağırarak daha hafif olabiliriz.

**Ses hattı**
- `AudioDeviceInput` her kayıtta 3 cihaz açıyor:
  - `SteelSeries Sonar - Gaming` (render cihazının loopback'i)
  - `SteelSeries Sonar - Chat` (render cihazının loopback'i)
  - `SteelSeries Sonar - Microphone` (capture)
- Media Foundation AAC encoder, 48 kHz stereo, 192 kbps, **her biri ayrı iz**
- Örnek klip (`ffprobe`): `video(h264) | Game(aac) | Chat(aac) | Mic(aac)` → **Media izi yok**, karışık (mix) iz de yok
- Loglarda çok sayıda `Dropped N audio packets in synchronizer` ve `Audio data ... way out of range` uyarısı var → **loopback'te sessizlik boşlukları ve saat kayması kaynaklı senkron sorunu**. Bizim tasarımda bunu baştan çözmemiz gerekiyor (bkz. 3.4).

**Mevcut ayarların** (`moments_settings`)
- Geriye dönük süre: **60 sn**, FPS: **60**, klasör: `E:\Records`, monitör: `\\.\DISPLAY2`
- 87 oyun tanımlı (hepsi `recording_mode = 2`), 430 klip, **hepsi kısayol ile tetiklenmiş** (otomatik/olay bazlı klip kullanılmıyor)
- Dosya adı formatı: `Oyun-Adi__YYYY-MM-DD__HH-mm-ss.mp4`

### 1.3 Sonar nasıl çalışıyor

Sonar üç katmandan oluşuyor:

**(a) Kernel sanal ses kartı — `SteelSeries-Sonar-VAD.sys`** (`ROOT\MEDIA\0000`)

| Endpoint | Yön | Rol (`PKEY_SS_VAD_ROLE`) |
|---|---|---|
| SteelSeries Sonar - Gaming | Render | 2 (Game) |
| SteelSeries Sonar - Chat | Render | 1 (Chat) |
| SteelSeries Sonar - Media | Render | 5 (Media) |
| SteelSeries Sonar - Aux | Render | 6 (sende devre dışı) |
| SteelSeries Sonar - Microphone | Capture (+ iç render ucu) | 3 (ChatCapture) |
| SteelSeries Sonar - Stream | Capture (+ iç render ucu) | 4 (Stream) |

Her endpoint'te `{F141E7CC-921B-466D-802F-879D6D4AEC17},0` property'si rolü taşıyor. Bu, endpoint'leri **isimden bağımsız** (dil/yeniden adlandırmadan etkilenmeden) tanımamızı sağlıyor.

**(b) Ses işleme eklentisi — `Sonar.APO.dll`** (A-Volute/Nahimic tabanlı)
- `audiodg.exe` içinde çalışan APO; sanal endpoint'lere EFX/MFX olarak bağlı
- EQ, sanal surround, compressor, noise gate, **ClearCast AI** gürültü engelleme (onnxruntime + DirectML ile)
- **Asıl yönlendirmeyi de bu APO yapıyor:** `StreamRedirection` / `SetOutputDevice` / `SetCopyOutputDevice` → APO, sanal kanaldaki sesi doğrudan fiziksel cihaza (Personal Mix) ve Stream cihazına (Stream Mix) kopyalıyor. Yani miksaj Windows ses motorunun içinde gerçekleşiyor, bu yüzden gecikme çok düşük.

**(c) Kontrol uygulaması — `SteelSeriesSonar.exe`** (.NET 8, yerel REST + WebSocket)
- Uygulama bazlı yönlendirme: `IAudioPolicyConfigFactory` → `SetPersistedDefaultAudioEndpoint` (Windows'un "Uygulama ses düzeyi ve cihaz tercihleri" sayfasının kullandığı belgelenmemiş API; Win11 21H2+ ve eski sürümler için iki ayrı arayüz varyantı tutuyor)
- Varsayılan cihaz: `IPolicyConfig::SetDefaultEndpoint` → Windows'ta varsayılanı Sonar - Gaming'e sabitliyor; fallback listeleriyle cihaz takılıp çıkarılınca yeniden atıyor. **Hoparlör seçememenin nedeni bu.**
- Yerel API (canlı sorguladım): `coreProps.json` → `https://127.0.0.1:6327/subApps` → Sonar adresi (her açılışta port değişiyor) → `/mode`, `/streamRedirections` vb.

**Senin canlı yönlendirmen** (`/streamRedirections` ve veritabanından):

| Mix | Hedef cihaz | Game | Chat | Media | Aux | Mic |
|---|---|---|---|---|---|---|
| **Personal (monitoring)** | HS80 Kablosuz | ✅ | ✅ | ✅ | ✅ | ❌ |
| **Stream (streaming)** | Sonar - Stream (sanal) | ✅ | ✅ | ✅ | ❌ | ✅ |
| **Mic** | HS80 Kablosuz mikrofonu → Sonar - Microphone | | | | | |

Mod: `stream` (Streamer modu). Tahminin doğru: Windows'ta "Sonar - Gaming" seçili görünüyor ama aslında Personal Mix HS80'e gidiyor.

---

## 2. Waveline Clip × Sonar uyumluluğu

**Kısa cevap: Evet, uyumlu çalışır.** Mevcut Moments de zaten aynı şekilde çalışıyor.

| İz | Sonar kuruluyken kaynak | Yöntem |
|---|---|---|
| 1 · Game | `Sonar - Gaming` render endpoint | WASAPI loopback |
| 2 · Chat | `Sonar - Chat` render endpoint | WASAPI loopback |
| 3 · Media | `Sonar - Media` render endpoint | WASAPI loopback (Moments'ın yapmadığı ek iz) |
| 4 · Mic | `Sonar - Microphone` capture endpoint | WASAPI capture (ClearCast/gate sonrası işlenmiş ses) |
| (ops.) Stream Mix | `Sonar - Stream` capture endpoint | Sonar'ın stream slider'larıyla hazır karışım |

Tasarım kararları:
1. **Kaynak soyutlaması:** Kayıt motoru "Game/Chat/Media/Mic" rollerini bir `AudioSourceProvider` arayüzünden alacak. İlk sağlayıcı **SonarProvider** (endpoint'leri `PKEY_SS_VAD_ROLE` ile bulur), ikincisi bizim Sonar'ımız için **OwnMixerProvider**, üçüncüsü Sonar yokken **ProcessLoopbackProvider** (Windows 11'in uygulama bazlı loopback'i ile oyun süreci → Game, Discord → Chat, tarayıcı/Spotify → Media). Böylece 2. aşamada Sonar'ı değiştirdiğimizde Moments'a dokunmayız.
2. **Dikkat — slider öncesi/sonrası:** Sanal endpoint loopback'i büyük olasılıkla Sonar slider'larından **önceki** sesi verir (slider'lar APO yönlendirmesinde uygulanıyor). Yani kayıttaki seviyeler Sonar'daki ayardan bağımsız olabilir. Prototipte ölçülecek; istenirse Stream Mix'i ayrıca 5. iz olarak almak bu farkı kapatır.
3. **Eşzamanlı çalışma:** Bizim uygulama çalışırken GG içindeki Moments kapatılmalı (çift NVENC oturumu ve çift kısayol çakışması olmasın). Sonar çalışmaya devam eder.
4. **Sonar yeniden başladığında/cihaz değiştiğinde** endpoint'ler kısa süre kaybolabilir → `IMMNotificationClient` ile izleyip ses akışlarını otomatik yeniden açacağız, eksik kalan süreyi sessizlikle dolduracağız (iz kaymasın).

---

## 3. Waveline Clip — teknik tasarım

### 3.1 Mimari

```
┌────────────── Tray UI (yalnızca açıldığında yüklenir) ─────────────┐
│ Ayarlar · kısayollar · klasör · durum                              │
└───────────────▲────────────────────────────────────────────────────┘
                │ named pipe (IPC)
┌───────────────┴──────────── Çekirdek (C++) ────────────────────────┐
│ GameDetector ──► SessionController ──► CaptureEngine               │
│  (olay tabanlı)      (state machine)     ├─ VideoSource (WGC/DXGI) │
│                                          ├─ GpuConvert (BGRA→NV12) │
│                                          ├─ NvencEncoder (SDK)     │
│                                          ├─ AudioCapture ×4        │
│                                          ├─ AacEncoder ×4 (MF)     │
│                                          ├─ ReplayRing (paket)     │
│                                          └─ Muxer (MP4/fMP4)       │
└────────────────────────────────────────────────────────────────────┘
```

### 3.2 Görüntü hattı (minimum yük)
1. **Yakalama:** Borderless/pencere → WGC pencere yakalama (sarı çerçeve kapalı, `IsBorderRequired = false`); masaüstü modu ve exclusive fullscreen → WGC monitör yakalama, olmazsa DXGI Desktop Duplication. Oyuna DLL enjekte eden "game hook" yöntemi **kullanılmayacak** (Vanguard/FACEIT/BattlEye riski).
2. **Renk dönüşümü GPU'da:** `ID3D11VideoProcessor` ile BGRA → NV12 (HDR için P010). CPU'ya hiçbir kare kopyalanmaz.
3. **Encoder:** NVENC SDK'yı doğrudan kullan (`NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX`, kayıtlı texture havuzu). RTX 5070 H.264 / HEVC / AV1 destekliyor.
   - Önerilen varsayılan: **HEVC** veya **AV1**, preset P4–P5, VBR/CQ, lookahead kapalı, **GOP = 1 sn** (kesme noktaları hassas olsun)
   - Uyumluluk modu: H.264 High
4. **Kare temposu:** Yakalama hedef FPS'te sınırlandırılır (oyun 143 FPS'te olsa da 60 FPS kodlanır); kare gelmezse son kare tekrarlanır (CFR).
5. **Beklenen yük:** NVENC ayrı bir donanım birimi olduğundan oyun FPS'ine etkisi genelde %1–3; CPU < %2. PresentMon ile ölçülecek.

### 3.3 Geriye dönük kayıt (replay buffer)
- Ham kare değil **kodlanmış paket** tutulur → çok düşük RAM.
- Boyut hesabı (1080p60): 20 Mbps ≈ 2.5 MB/sn → **5 dk ≈ 750 MB, 10 dk ≈ 1.5 GB, 20 dk ≈ 3 GB** (+ ses ~0.1 MB/sn/iz)
- İki mod:
  - **RAM halkası:** ≤ ~10 dk için (32 GB RAM'de rahat)
  - **Disk halkası:** daha uzun süreler için `E:\` üzerinde 2 sn'lik segmentler, eskiler döngüsel silinir
- Süre seçimi: arayüzde dakika seçici (ör. 0.5 / 1 / 2 / 5 / 10 / 15 / 20 dk). Çalışırken değiştirilebilir; kısaltma anında, uzatma ileriye doğru dolarak uygulanır.
- **Kaydet:** kısayola basınca halkanın anlık görüntüsü alınır, istenen süre öncesindeki ilk keyframe'den başlanarak **yeniden kodlamadan** MP4'e yazılır (düşük öncelikli arka plan iş parçacığında, birkaç saniye).

### 3.4 Ses hattı (4 iz, senkron sorunsuz)
- 4 adet WASAPI shared, **event-driven** akış; her biri MMCSS "Pro Audio" önceliğinde.
- **Zaman damgası tabanlı birleştirme:** Her paket QPC zamanıyla işaretlenir. Loopback sessizken paket gelmez → aradaki süre **sessizlikle doldurulur** (Moments'taki `Dropped packets` sorununun kaynağı burası). Saat kayması küçük bir resampler ile düzeltilir.
- Kodlama: Media Foundation AAC-LC 48 kHz, 160–192 kbps (Windows'ta yerleşik, ek lisans gerekmez).
- İz sırası: **1 Game · 2 Chat · 3 Media · 4 Mic**, her izin `handler_name`'i rol adıyla yazılır (Premiere/DaVinci'de isimli görünür).
- Not: Windows medya oynatıcıları ve Discord çoğu zaman yalnızca ilk ses izini çalar. İstersen 0. iz olarak bir "Mix" izi eklenebilir (Soru 3).

### 3.5 Oyun / tam ekran algılama (olay tabanlı, polling yok)
- `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_LOCATIONCHANGE)` ile ön plan penceresi takibi.
- **Borderless fullscreen:** pencere dikdörtgeni = monitör dikdörtgeni ve `WS_CAPTION` / `WS_THICKFRAME` yok.
- **Exclusive fullscreen:** `SHQueryUserNotificationState` → `QUNS_RUNNING_D3D_FULL_SCREEN`.
- Yanlış pozitifleri önleme: tarayıcı/medya oynatıcı kara listesi (YouTube tam ekran da borderless görünür), kullanıcı beyaz/kara listesi, isteğe bağlı GPU kullanımı kontrolü (Windows'un süreç bazlı "GPU Engine" performans sayaçlarıyla; oyun sürecine handle açmadan).
- ⚠️ Oyun sürecinin modüllerini okumak (`EnumProcessModules`, `PROCESS_VM_READ`) **yasak** — anti-cheat'ler bunu hile davranışı olarak görür (bkz. Bölüm 7.1).
- **Masaüstü modu:** oyun yokken açıksa ilgili monitör WGC monitör yakalama ile buffer'a alınır; oyun açılınca otomatik pencere moduna geçer.
- Boşta (oyun yok, masaüstü modu kapalı): yakalama ve encoder tamamen kapalı, yalnızca olay kancası çalışır → ~0 CPU.

### 3.6 Manuel kayıt
- Aynı encoder oturumundan beslenir (ikinci NVENC oturumu açılmaz) → buffer ve manuel kayıt **aynı anda** çalışabilir.
- Diske fMP4/MKV olarak yazılır (çökmede dosya kurtulur), bitişte MP4'e remux edilir.
- Başlat/durdur kısayolu; isteğe bağlı otomatik bölme (ör. her 30 dk).

### 3.7 Kısayol ve bildirim
- `RegisterHotKey` ile global kısayollar (Moments'taki gibi).
- Bildirim: ses efekti (`clip-taken.wav` benzeri) + Windows toast. Oyuna overlay enjekte edilmeyecek (anti-cheat).

### 3.8 OBS ile birlikte çalışma (çift kaynak kullanımını önleme)

**Sorun:** OBS açıkken iki ayrı hat çalışır: iki yakalama, iki renk dönüşümü, iki NVENC oturumu. RTX 5070'te tek NVENC birimi var; iki 1080p60 kodlama rahat sığar, oyun FPS'ine etkisi küçüktür. Yine de NVENC birimi, VRAM (birkaç yüz MB) ve az miktarda CPU iki kez harcanır. Gerçek değerler M0'da ölçülecek.

**Neden tek kodlama her zaman paylaşılamaz:** OBS'nin kodladığı görüntü sahnedir (overlay, kamera, uyarılar). Temiz oyun klibi istiyorsan iki ayrı kodlama zorunludur. Klipte OBS sahnesinin olması kabulse, tek kodlamaya inilebilir.

**Çözüm — üç çalışma modu (ayarlardan seçilir):**

| Mod | OBS açıkken ne olur | Yük | Klip içeriği |
|---|---|---|---|
| **A. Bağımsız** | Kendi hattımız çalışmaya devam eder, NVENC önceliği düşürülür (daha hızlı preset) | ~2x encoder, FPS etkisi düşük | Temiz oyun görüntüsü |
| **B. OBS köprüsü (önerilen)** | Kendi hattımız durur; kısayol, OBS'nin **Replay Buffer**'ını `obs-websocket` (OBS 28+ içinde gelir) üzerinden kaydettirir; dosya oyun adıyla `E:\Records`'a taşınır | Ek yük **yok** | OBS'nin kaydettiği içerik |
| **C. Paylaşılan yakalama** | Bizim motor oyunu bir kez yakalar, OBS'ye paylaşılan D3D11 texture'ı olarak kaynak verir (OBS eklentisi) | Yakalama 1x, kodlama 2x | Temiz klip + OBS sahnesi |

> ✅ **Karar (22.11.2026):** Mod B seçildi ama **sonraki plana (ilk sürüm sonrası)** bırakıldı. Geçiş, OBS'nin açılmasıyla değil, **yalnızca OBS'de yayın veya kayıt başladığında** yapılacak; yayın/kayıt bitince kendi motorumuza dönülecek.

**Mod B ayrıntıları**
- Tetikleyici: websocket'in `StreamStateChanged` / `RecordStateChanged` olayları. Yayın/kayıt başlayınca köprüye geçilir, bitince kendi motorumuz devam eder.
- OBS'de "Gelişmiş çıktı"da kayıt encoder'ı **"Yayın encoder'ını kullan"** seçilirse yayın + kayıt + replay buffer **tek NVENC oturumunu** paylaşır.
- Ses izleri: OBS 6 iz destekler; Game/Chat/Media/Mic kaynakları iz 1–4'e eşlenir (kurulum sihirbazı bunu websocket ile yapabilir).
- Geriye dönük süre OBS profilinde tutulur; değiştirmek replay buffer'ın kısa süre yeniden başlatılmasını gerektirir.
- Geçiş anı: OBS'nin buffer'ı dolana kadar kendi halkamızı tutup sonra kapatmak (kısa süreli çift yük) ya da boşluğu kabul etmek — Soru 16.

### 3.9 Dayanıklılık
- GPU device lost / çözünürlük değişimi / alt-tab / uyku–uyanma / monitör değişimi / ses cihazı takılıp çıkarma senaryoları.
- Disk dolu koruması, kayıt klasörü erişilemezse uyarı.

---

## 4. Waveline Mix — teknik tasarım

### 4.1 En kritik karar: sanal ses cihazı

Game/Chat/Media'yı ayırabilmek için uygulamaların sesini **gönderebileceği ayrı endpoint'ler** gerekiyor; bu da bir kernel ses sürücüsü demek. Windows 11 64-bit, imzasız kernel sürücüsü yüklemez.

| Seçenek | Artı | Eksi |
|---|---|---|
| **A. Kendi VAD sürücümüz** (Microsoft SYSVAD örneğinden) | Tam kontrol, rol property'leri, istediğimiz kadar endpoint | **EV kod imzalama sertifikası + Microsoft attestation imzası** gerekir (yıllık ücretli). Test-signing modu anti-cheat'leri (Vanguard, FACEIT, BattlEye) bozar → oyun oynayan biri için pratik değil |
| **B. Hazır imzalı sanal kablo sürücüsü** (VB-Audio Cable vb.) | Hemen başlanır, imza derdi yok | Endpoint sayısı/isim kısıtlı, lisans (dağıtım) koşulları, rol property'si yok |
| **C. SteelSeries VAD'ını kullanmaya devam** | Sıfır sürücü işi | Ticari/kapalı sürücü, GG kaldırılınca biter, güncellemelerde bozulabilir — **önerilmez** |

**Öneri:** Prototipi **B** ile (veya kişisel kullanım için geçici olarak C ile) başlatıp mikser/yönlendirme/UI'ı geliştirmek; sürücü katmanını bir arayüz arkasında tutmak; dağıtım hedefleniyorsa paralelde **A** için imzalama sürecini başlatmak. (Soru 2)

### 4.2 Miksaj motoru (user-mode)
- Sonar bunu APO ile `audiodg` içinde yapıyor; biz ilk sürümde **kullanıcı modunda** yapacağız (APO'nun da imzalı sürücü paketiyle kurulması gerekiyor ve hata ayıklaması zor).
- Akış: her sanal render endpoint → (loopback veya sürücünün paylaşılan halka belleği) → kanal başına **iki gain** (Personal, Stream) → 
  - **Personal Mix** → seçili fiziksel cihaz (WASAPI event-driven, ~10 ms tampon)
  - **Stream Mix** → sanal "Stream" capture endpoint (OBS buradan alır)
- Ek gecikme hedefi: < 20 ms (APO'ya göre biraz fazla; oyun için hissedilmez seviyede tutulacak).
- Kanal başına mute, Personal/Stream bağımsız slider, master slider; ayarlar anında uygulanır (yumuşatılmış gain → tık sesi yok).

### 4.3 Uygulamaları katmana sürükle-bırak
- Aktif ses oturumları `IAudioSessionManager2` + `IAudioSessionNotification` ile listelenir (ikon, süreç adı, seviye göstergesi).
- Bırakınca: `IAudioPolicyConfigFactory::SetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia/eConsole, endpoint)`; Chat için mikrofonda `eCapture` + `eCommunications`.
- Win11'de çoğu uygulama anında taşınır; bazıları akışı yeniden açana kadar bekler.
- Kural: uygulama içinde **belirli** bir cihaz seçilmişse (ör. Discord çıkışı "HS80") Windows yönlendirmesi işlemez → Discord'da çıkış "Varsayılan" veya "Chat" olmalı. UI bunu algılayıp uyarabilir.
- Kalıcılık: exe yoluna göre kural tablosu; uygulama yeniden açıldığında otomatik uygulanır.

### 4.4 Mikrofon kanalı
- Fiziksel mikrofon (HS80) → DSP zinciri → sanal "Microphone" endpoint.
- Temel zincir: **Gain (dB, ör. −12…+24)** → **Noise Gate** (eşik dB, attack, hold, release; Sonar'daki gibi otomatik eşik opsiyonu) → isteğe bağlı High-pass (80 Hz) → isteğe bağlı Limiter (clipping'e karşı).
- Opsiyonel: RNNoise ile hafif yapay zekâ gürültü bastırma (BSD lisans, düşük CPU). (Soru 10)
- Canlı seviye göstergesi + gate eşiği çizgisi.

### 4.5 Windows hoparlör seçimi (istediğin "gizleme + otomatik atama")

Açık konuşmak gerekirse: Windows, **aktif** bir endpoint'i hoparlör listesinden gizlemeye izin veren bir mekanizma sunmuyor; devre dışı bırakılan endpoint'e de uygulamalar ses gönderemez. Bu yüzden iki uygulanabilir yol var:

**Yol 1 — Takip et ve geri al (✅ SEÇİLDİ, 22.11.2026)**
- `IMMNotificationClient::OnDefaultDeviceChanged` dinlenir.
- Kullanıcı Windows'ta "HS80" gibi fiziksel bir cihaz seçerse: Personal Mix çıkışı HS80 yapılır, varsayılan cihaz anında tekrar sanal Game'e döndürülür.
- Sanal Game endpoint'inin adı dinamik güncellenir (ör. "**HS80 Kablosuz · Mixer**") → listede seçili görünen isim hep gerçek hoparlörünü gösterir.
- Chat/Media/Stream endpoint'leri listede kalır ama isimleri ayırt edici olur (ör. "· Chat (Mixer)").
- Not: endpoint adını yazmak yönetici yetkisi isteyebilir → arka planda küçük bir servis.

**Yol 2 — Game katmanı fiziksel cihaz olsun (deneysel, prototipte doğrulanmalı)**
- Sanal cihazlar yalnızca Chat, Media, Mic, Stream. Varsayılan hoparlör = gerçek cihaz, Windows seçimi doğal çalışır.
- Game sesi Stream Mix için Windows 11 process-loopback'in "kendi mikser sürecimiz hariç" moduyla alınır.
- Eksisi: Personal Mix'teki Game slider'ı Windows ses seviyesi olur, Game'e özel EQ olmaz, process-loopback davranışı farklı endpoint'lerde test edilmeli.

### 4.6 Güvenlik ağları
- Uygulama kapanırsa/çökerse varsayılan cihazı ve uygulama yönlendirmelerini **son fiziksel cihaza geri al** (aksi halde ses tamamen kesilir). Kaldırıcı da aynısını yapmalı.
- Cihaz takılıp çıkarma için fallback listesi (Sonar'daki `*_FALLBACK_DEVICES` mantığı).

---

## 5. Teknoloji önerisi

| Katman | Öneri | Neden |
|---|---|---|
| Kayıt motoru, mikser | **C++20** (D3D11, NVENC SDK, WASAPI, Media Foundation) | COM/D3D/NVENC API'lerine doğrudan erişim, en düşük yük |
| Arayüz | **C# .NET 8 (WinUI 3 veya WPF)**, yalnızca açıldığında çalışan ayrı süreç | Sürükle-bırak, slider, tray kolay; kapalıyken 0 RAM |
| Alternatif | Tamamen Rust (windows-rs) | Güvenli bellek, ama NVENC/WASAPI örnekleri C++'ta çok daha bol |
| IPC | Named pipe + küçük JSON/MessagePack protokolü | Basit, hızlı |
| Ayar/kütüphane | JSON ayar dosyası + (gerekirse) SQLite | |
| Muxer | Kendi fMP4 yazıcımız veya yalnızca `libavformat` (encoder'sız) | FFmpeg'in tamamına gerek yok |

Önerilen depo yapısı:

```
/core            C++ ortak: log, ipc, ayarlar, endpoint keşfi
/moments-engine  yakalama, encoder, ses izleri, replay ring, mux
/mixer-engine    sanal cihaz arayüzü, mikser, DSP, yönlendirme
/ui              C# arayüz (iki uygulama için ortak kabuk)
/driver          (ileride) VAD sürücüsü
/tools           benchmark (PresentMon), test sinyali üreticileri
```

---

## 6. Yol haritası

### Aşama 1 — Waveline Clip

| # | Kilometre taşı | Çıktı / kabul kriteri |
|---|---|---|
| M0 | **Teknik prototip** | WGC → GPU NV12 → NVENC → 30 sn MP4; 4 Sonar endpoint'inden ses; oyun FPS kaybı ve CPU ölçümü raporu |
| M1 | Replay buffer çekirdeği | RAM halkası, dakika seçimi, kısayolla kaydet, keyframe hizalı kesim, 4 isimli iz, A/V senkron farkı < 20 ms (10 dk test) |
| M2 | Otomatik algılama | Borderless/exclusive/pencere tespiti, kara/beyaz liste, masaüstü modu, otomatik geçişler |
| M3 | Manuel kayıt | Buffer ile eşzamanlı, çökmeye dayanıklı dosya, remux |
| M4 | Arayüz | Tray, ayarlar, kısayol atama, bildirim, Windows ile başlat |
| (sonraki plan) | OBS entegrasyonu | Mod B köprü: yayın/kayıt başlayınca devreye girer (websocket, iz eşleme, otomatik geçiş) |
| M5 | Sağlamlaştırma | Disk halkası, cihaz/GPU/çözünürlük olayları, uzun süreli (8 saat) sızıntı testi, Sonar yeniden başlatma testi |

### Aşama 2 — Waveline Mix

| # | Kilometre taşı | Çıktı / kabul kriteri |
|---|---|---|
| S0 | Sürücü kararı + prototip | Seçilen sanal cihaz yöntemiyle 4 render + 2 capture endpoint |
| S1 | Mikser motoru | Personal ve Stream mix, kanal başına iki slider, ek gecikme < 20 ms, sıfır çatırtı |
| S2 | Uygulama yönlendirme | Sürükle-bırak UI, kalıcı kurallar, Discord uyarısı |
| S3 | Mikrofon DSP | Gain, noise gate (+ ops. HPF, limiter, RNNoise), seviye göstergesi |
| S4 | Varsayılan cihaz yönetimi | 4.5'teki seçilen yol, çökme/kaldırma geri alma |
| S5 | Geçiş | GG/Sonar kaldırıldığında temiz geçiş; Waveline Clip'in `OwnMixerProvider`'a geçmesi |

---

## 7. Riskler

| Risk | Etki | Önlem |
|---|---|---|
| Sürücü imzalama | Waveline Mix dağıtılamaz / anti-cheat çakışması | 4.1'deki aşamalı yaklaşım |
| Belgelenmemiş `IAudioPolicyConfig` API'si | Windows güncellemesinde kırılabilir | Sonar gibi sürüm varyantlarını tutmak, başarısızlıkta kullanıcıya bildirmek |
| Exclusive fullscreen | WGC bazı oyunlarda siyah kare verebilir | DXGI Duplication fallback; Win11'de "pencereli oyunlar için iyileştirmeler" açık tutulmalı |
| Ses senkronu | Mevcut Moments'taki kayma sorunları | QPC tabanlı birleştirme + sessizlik doldurma + drift düzeltme |
| Anti-cheat | Enjeksiyon/overlay ban riski | Hiçbir oyuna DLL enjekte edilmeyecek |
| Parsec Virtual Display | Yanlış monitörün seçilmesi | Monitör seçimi EDID/isimle, oyunun bulunduğu monitörü otomatik izleme |

### 7.1 Anti-cheat uyumluluk kuralları (tüm kod için zorunlu)

%100 garanti verilemez: ban kararı anti-cheat firmasınındır ve kurallarını yayınlamazlar. Ama ban'a yol açan davranışlar bilinir ve bunların hiçbiri gerekmez.

| # | Kural | Neden |
|---|---|---|
| 1 | Oyun sürecine **asla** `PROCESS_VM_READ/WRITE`, `PROCESS_ALL_ACCESS` vb. ile handle açılmaz; en fazla `PROCESS_QUERY_LIMITED_INFORMATION` (exe yolu için, Görev Yöneticisi'nin kullandığı seviye) | Bellek okuma/yazma, hilelerin temel imzası |
| 2 | Oyuna DLL enjeksiyonu, D3D/Present hook'u, in-game overlay **yok** | Hook/enjeksiyon doğrudan tespit edilir |
| 3 | Görüntü yalnızca WGC / DXGI Duplication ile alınır | OBS, Discord, Xbox Game Bar'ın kullandığı işletim sistemi API'leri |
| 4 | Ses yalnızca WASAPI ve Windows'un uygulama başına cihaz ayarı ile yönetilir | Oyun sürecinin dışında, Windows Ayarlar'ın yaptığı işlem |
| 5 | Kısayollar `RegisterHotKey` ile; `WH_KEYBOARD_LL`/`WH_MOUSE_LL` low-level hook ve `SendInput` ile girdi enjeksiyonu **yok** | Low-level hook ve girdi otomasyonu makro/hile ile karıştırılır |
| 6 | Oyun olayına bağlı otomatik klip istenirse yalnızca resmi entegrasyonlar (ör. CS2 Game State Integration) kullanılır | Oyun belleğinden olay okumak = hile |
| 7 | Sürücü: test-signing modu **asla**; yalnızca Microsoft imzalı (attestation) sürücü veya yaygın imzalı sanal kablo | Test modunda Vanguard/FACEIT çalışmaz |
| 8 | Kendi sürücümüz yazılırsa: yalnızca ses işi yapar, rastgele bellek/port erişimi veren IOCTL yok, cihaz nesnesine sıkı ACL | Açıklı imzalı sürücüler (BYOVD) Microsoft'un ve anti-cheat'lerin engel listesine girer |
| 9 | Kendi exe'lerimiz kod imzalı olur (mümkünse) | Güvenilirlik/SmartScreen; şart değil ama tavsiye edilir |

Doğrulama: her sürüm, oynanan anti-cheat'li oyunlarda (CS2/VAC, R6/BattlEye ve varsa FACEIT/Vanguard) önce casual/deathmatch modlarında denenir; anti-cheat'in oyunu açmayı reddetmesi veya uyarı vermesi izlenir.

---

## 8. Açık sorular

**Genel**
1. **Dil/teknoloji:** C++ çekirdek + C# arayüz önerisi uygun mu? Yoksa tercih ettiğin bir dil (C#, Rust, C++) veya arayüz kütüphanesi var mı?
2. **Dağıtım ve sürücü:** Bu uygulamalar sadece senin için mi, yoksa başkalarına da dağıtılacak mı (açık kaynak / ücretli)? Buna göre EV sertifika + Microsoft imzası mı alınacak, hazır sanal kablo sürücüsüyle mi başlanacak?

**Moments**
3. **Klip formatı:** Codec (H.264 her yerde açılır / HEVC–AV1 daha küçük), FPS, bitrate/kalite, HDR gerekli mi? İlk izde Game yerine **tüm seslerin karışımı** (Mix) olsun mu, sonra 4 ayrı iz?
4. **Geriye dönük süre:** Seçilebilir dakika aralığı ne olsun (ör. 30 sn – 20 dk)? En uzun süre ~10 dakikayı geçecekse disk halkası kabul mü?
5. **Manuel kayıt:** Buffer'la aynı anda çalışsın mı? Kalite ayarları buffer'dan farklı olabilmeli mi?
6. **Bildirim:** Klip alındığında ses, Windows bildirimi, ikisi birden?
7. **Algılama kuralları:** Tam ekran olan her uygulama mı kaydedilsin, yoksa bir oyun listesi mi (mevcut 87 oyunluk listeyi içe aktaralım mı)? Tarayıcıda tam ekran video izlerken kayıt başlamamalı, değil mi?
8. **Çoklu monitör:** Masaüstü modunda hangi monitör (şu an DISPLAY2), yoksa farenin/aktif pencerenin bulunduğu monitör mü?
9. **Kütüphane/editör:** Moments'taki klip galerisi, kırpma, paylaşma gibi özellikler istiyor musun, yoksa sadece `E:\Records` klasörüne dosya yazması yeterli mi? Mevcut 430 klibin kütüphanesi taşınsın mı?
10. **Mic izi:** Kayda işlenmiş (gate/ClearCast sonrası) mikrofon mu girsin, ham mikrofon mu?

**Sonar**
11. **Ek özellikler:** Aux kanalı, EQ, sanal surround veya yapay zekâ gürültü bastırma ilk sürümde gerekli mi, yoksa yalnızca gain + noise gate yeterli mi?
12. **Stream Mix:** Kim kullanıyor (OBS, Discord yayını)? Stream Mix'i ayrıca kulaklıktan dinleme ("stream monitoring") gerekli mi?
13. **Hoparlör seçimi:** 4.5'teki **Yol 1** (dinamik isim + otomatik geri alma) mi, **Yol 2** (Game = gerçek cihaz, deneysel) mi?
14. **Yetkiler:** Uygulamanın küçük bir arka plan servisi kurması ve ilk kurulumda yönetici izni istemesi sorun olur mu?
15. **Geçiş dönemi:** 1. aşamada GG'deki Moments'ı kapatıp Sonar'ı açık bırakmak uygun mu?

**OBS**
16. OBS açıkken klipte **temiz oyun görüntüsü** mü olsun (Mod A/C), yoksa OBS sahnesi de kabul mü (Mod B, ek yük yok)? OBS'yi yayın mı, kayıt mı, ikisi için mi kullanıyorsun? Moda geçerken birkaç dakikalık geriye dönük boşluk kabul edilir mi?
