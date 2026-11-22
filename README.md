# Waveline

SteelSeries Moments ve Sonar'ın hafif, açık kaynak karşılıkları.

| Bileşen | Karşılığı | Durum |
|---|---|---|
| **Waveline Clip** | Moments: geriye dönük kayıt + manuel kayıt, 5 ses izi | M5 (tepsi + ayarlar, sağlamlaştırma) |
| **Waveline Mix** | Sonar: Game / Chat / Media / Mic katmanları, Personal + Stream Mix | Planlandı |

Analiz, mimari ve kararlar: [ANALIZ_VE_PLAN.md](ANALIZ_VE_PLAN.md)

---

## Gereksinimler

- Windows 11, NVIDIA GPU (NVENC)
- Visual Studio 2026 (veya 2022) ve şu bileşenler:
  - **Desktop development with C++** iş yükü
  - **Windows 11 SDK (10.0.26100 veya daha yeni)** ← C++/WinRT başlıkları bununla gelir

## Derleme

```powershell
.\build.ps1
```
Betik C++ motorunu (MSBuild) ve ayarlar penceresini (.NET 8) derler. Çıktılar `build\x64\Release\` altına gider:
- `WavelineClip.exe` — arka plan motoru (tepsi simgesi)
- `WavelineSettings.exe` — ayarlar penceresi
- `WavelineClipProto.exe` — M0 ölçüm prototipi

Klasör yapısı:
- `src\clip-core\` — ortak motor: yakalama, GPU dönüşümü, NVENC, ses, halka, MP4, manuel kayıt
- `src\clip\` — WavelineClip: algılama, oturumlar, tepsi, ayarlar, bildirim sesi
- `src\settings\` — WPF ayarlar penceresi
- `src\clip-proto\` — M0 prototipi

---

## `WavelineClip`

`WavelineClip.exe`'yi çalıştırın; sistem tepsisine yerleşir ve arka planda bekler. Oyun açılınca kendiliğinden kayda başlar. Son N dakikayı RAM'de sıkıştırılmış paketler olarak tutar.

| Kısayol | İşlev |
|---|---|
| **Ctrl+Alt+F10** | Son N dakikayı klip olarak kaydet (yeniden kodlamadan, ~0.1–0.4 sn) |
| **Ctrl+Alt+F9** | Manuel kaydı başlat / durdur |

**Tepsi simgesi**
- **Renkler:** Gri = oyun bekleniyor, turkuaz = geriye dönük kayıt açık, kırmızı = manuel kayıt sürüyor.
- **Bilgi:** Üzerine gelince oyun adı, tampon süresi ve kayıt süresi görünür.
- **Menü (sağ tık):** Klip kaydet, Kaydı başlat/durdur, Klasörü aç, Ayarlar…, Çıkış.
- **Tıklama:** Simgeye tıklamak ayarları açar. Bildirime tıklamak dosyayı Gezgin'de gösterir.
- **İkinci başlatma:** Program zaten çalışırken tekrar açılırsa ikinci bir kopya başlamaz, ayarlar penceresi açılır.

**Ayarlar**
- **Ayar dosyası:** `%APPDATA%\Waveline\clip.json`. Ayarlar penceresi bu dosyayı düzenler; motor değişikliği ~1 sn içinde **yeniden başlatmadan** uygular.
- **Video ayarları:** Süre, klasör, codec ve FPS gibi ayarlar değişirse süren kayıt kapatılıp yeni ayarlarla devam edilir.
- **Windows ile başlat:** Ayarlardan açılıp kapatılır (`HKCU\...\Run`).
- **Log:** `%LOCALAPPDATA%\Waveline\clip.log`. Ayarlar penceresindeki "Log" düğmesiyle açılır.

**Bildirim sesi kliplere girmez:** Sesler Sonar'ın sanal kanallarına değil, doğrudan kulaklığa çalınır.
- **Otomatik seçim:** "Otomatik" seçeneği, Sonar'ın yerel API'sinden Personal Mix cihazını sorar (ör. HS80). Sonar yoksa varsayılan çıkış kullanılır.
- **Güvenlik kuralı:** Uygun cihaz bulunamazsa ses çalınmaz; kliplere girmesindense hiç çalmaması tercih edilir.
- **Windows bildirimi:** Bildirimler de sessizdir.

**Nasıl çalışır**
- **Oyun algılama:** Ön plandaki pencere monitörü başlık çubuğu olmadan kaplıyorsa (borderless/fullscreen) ya da Windows exclusive fullscreen bildiriyorsa, ve 1.5 sn böyle kalırsa oyun sayılır.
- **Hariç tutulanlar:** Tarayıcılar, medya oynatıcılar, Discord, OBS, Explorer vb. tam ekran olsa da kaydedilmez. Ayarlardan liste eklenebilir; pencereli oyunlar "Pencereli de olsa kaydet" listesine yazılır.
- **Kayıt süresi:** Oyun penceresi kapanana kadar kaydedilir; alt-tab yapınca kayıt durmaz.
- **Masaüstü modu:** Ayarlardan açılır; oyun yokken seçili monitör kaydedilir. Parsec gibi sanal ekranlar otomatik seçilmez.
- **Boşta yük:** Oyun yokken (ve masaüstü modu kapalıyken) yakalama ve encoder tamamen kapalıdır. Ölçülen: ~17 MB RAM, CPU ~%0.02. Ayarlar penceresi yalnızca açıkken çalışır.
- **Anti-cheat:** Oyun algılama oyuna dokunmaz. Pencere olayları süreç dışından dinlenir, exe adı en düşük yetkiyle okunur.

Komut satırı (tanı/test): `--console` (log'u konsola da yaz), `--list`, `--auto-save SN`, `--exit-after SN`, `--test-record A:B`.
### Manuel kayıt (M3)

Kayıt yapılırken (oyun ya da masaüstü) **Ctrl+Alt+F9** ile başlar, tekrar basınca durur.
- **Encoder:** Geriye dönük tamponu besleyen encoder'ı kullanır; ikinci bir NVENC oturumu açılmaz.
- **Eşzamanlılık:** Kayıt sürerken Ctrl+Alt+F10 ile klip almaya devam edebilirsiniz.
- **Başlangıç anı:** Kayıt, tamponda bekleyen son keyframe'den başlar; yani tuşa basmadan en fazla ~1 sn öncesi de kayda girer.
- **Dosya:** `oyunexe__kayit__YYYY-MM-DD__HH-mm-ss.mp4` olarak yazılır ve kayıt durduğu anda hazırdır; dönüştürme beklenmez. ayarlardaki bölme seçeneğiyle `_2`, `_3`… parçalarına bölünür.
- **Kaydın bitişi:** Oyun kapanınca kayıt da kendiliğinden durur.
- **Çökme yedeği ve otomatik kurtarma:** Kayıt sürerken yanında `...kayit....parts\` klasörüne 10 saniyelik, tek başına oynatılabilen yedek bölümler yazılır; program normal kapanırsa bu klasör silinir. Program ya da bilgisayar çökerse, Waveline Clip bir sonraki açılışta yarım kalan kaydı bu bölümlerden **kendiliğinden** aynı isimle yeniden oluşturur ve bildirim gösterir. En fazla son ~10 sn kaybolur.

### Dayanıklılık (M5)

| Durum | Davranış |
|---|---|
| Sonar yeniden başladı / kulaklık çıkıp takıldı | İlgili ses izi saniyede bir yeniden bağlanmayı dener, cihazı Sonar rolüyle yeniden bulur. Aradaki süre sessizlikle dolar, senkron kaymaz |
| Uykudan uyanma | Kayıt temiz şekilde yeniden başlar |
| Monitör düzeni değişti (masaüstü modu) | Masaüstü kaydı yeniden başlar; oyun içi çözünürlük değişikliği kesintisiz işlenir |
| GPU sürücüsü sıfırlandı / yakalama hatası | Oturum kapanır, oyun hâlâ açıksa ~1.5 sn içinde yeniden algılanıp kayıt sürer |
| Disk dolu | Klip, boyutu kadar yer yoksa kaydedilmez. Manuel kayıt 1 GB'tan az yer kalınca dosyayı düzgün kapatıp durur. Her ikisinde bildirim çıkar |
| Program/PC çöktü | Manuel kayıt bir sonraki açılışta otomatik kurtarılır |

Klip özellikleri:
- **Klasörleme:** Her oyunun klipleri ve kayıtları kendi alt klasörüne gider: `E:\Clips\Counter-Strike 2\Counter-Strike-2__2026-11-22__20-15-00.mp4`. Masaüstü kayıtları `Masaüstü\` klasörüne yazılır. Oyun adı exe dosyasının ürün bilgisinden okunur (ör. `cs2.exe` → "Counter-Strike 2"); bulunamazsa pencere başlığı, o da yoksa exe adı kullanılır.
- **Etiketler:** Her dosyaya oyun adı yazılır.
  - Windows Gezgini'nde **Etiketler**, **Tür**, **Albüm**, **Başlık** ve **Açıklamalar** olarak görünür. Gezgin'de bu sütunlara göre sıralayabilir ve arayabilirsiniz.
  - Oynatıcılarda ve düzenleyicilerde de standart MP4 etiketleri olarak görünür (title/album/genre/comment/encoder/date).
- **İzler:** Ses izleri isimli: `Mix`, `Game`, `Chat`, `Media`, `Mic`.
- **Dosya kimliği ve düzenleme bilgisi:** Her dosyada Waveline'a ait gizli etiketler de var. Dosyayla birlikte taşınırlar; ffprobe'da görünürler.
  - `id`: dosyaya özel benzersiz kimlik. Taşınsa da, adı değişse de aynı kalır.
  - `kind`: `clip` ya da `recording`.
  - `game`: oyun adı.
  - `edit`: ileride kütüphane/düzenleyicinin kullanacağı düzenleme bilgisi (JSON): favori, kırpma başı/sonu, her ses izinin seviyesi ve sessizliği. Bu bilgi dosyanın görüntü/ses verisine dokunmadan, sadece etiket yeniden yazılarak değiştirilir.

  ```powershell
  .\build\x64\Release\WavelineClip.exe --tags "E:\Clips\Counter-Strike 2\klip.mp4"            # etiketleri göster
  .\build\x64\Release\WavelineClip.exe --favorite "E:\Clips\Counter-Strike 2\klip.mp4" on     # favoriye al
  ```
- **Süre:** Her saniyede bir keyframe olduğu için klip istenen süreden en fazla ~1 saniye uzun olur.
- **Senkron:** Ses ve video aynı noktada biter.

Not: Klip alındı sesi varsayılan ses cihazından (Sonar - Gaming) çaldığı için bir sonraki klibin Game izine de girer. M4'te bu ses kayıt dışı bir yoldan çalınacak.

---
## M0 prototipi: `WavelineClipProto`

Amaç: kayıt hattının düşük yükle çalıştığını kanıtlamak ve maliyetini ölçmek.

```
Windows.Graphics.Capture ──► GPU'da BGRA→NV12 (D3D11 Video Processor) ──► NVENC (HEVC/H.264)
WASAPI: Sonar Gaming / Chat / Media (loopback) + Sonar Microphone ──► Mix + 4 iz ──► AAC
                                                                   └─► MP4 (1 video + 5 ses izi)
```

- Kareler hiç CPU'ya kopyalanmaz.
- Oyun 143 FPS'te çalışsa bile yalnızca hedef FPS kadar (ör. 60) kare işlenir.
- Ses izleri QPC zaman damgalarıyla hizalanır; sessizlikte gelmeyen paketler sessizlikle doldurulur (Moments'taki kayma sorununa karşı).
- Sonar cihazları isimle değil, sürücünün yazdığı rol bilgisiyle bulunur.

### Kullanım

```powershell
.\build\x64\Release\WavelineClipProto.exe --list
```
Monitörleri, ses cihazlarını (Sonar rolleriyle) ve donanım encoder'larını listeler.

```powershell
.\build\x64\Release\WavelineClipProto.exe --window 5 --seconds 60 --out E:\Records\m0_test.mp4
```
5 saniye içinde oyuna geçin; o pencere 60 sn kaydedilir.

| Seçenek | Açıklama |
|---|---|
| `--monitor N` | N numaralı monitörü kaydet (varsayılan: ana monitör) |
| `--window [sn]` | sn saniye sonra öne gelen pencereyi kaydet |
| `--seconds N` | Süre (varsayılan 30, Ctrl+C ile erken bitir) |
| `--fps N` | Varsayılan 60 |
| `--bitrate KBPS` | Varsayılan 15000 |
| `--codec hevc\|h264` | Varsayılan hevc |
| `--no-cursor` | İmleci kaydetme |
| `--out DOSYA.mp4` | Çıktı dosyası |

Kayıt sırasında her saniye CPU, NVENC, GPU 3D, RAM ve VRAM kullanımı yazdırılır. Sonunda video dosyasının yanına `*.m0.txt` raporu kaydedilir.

### M0 test prosedürü

1. GG içindeki **Moments'ı kapatın** (Sonar açık kalsın).
2. `--list` çıktısında Sonar cihazlarının rollerinin göründüğünü ve bir NVIDIA HEVC encoder'ı listelendiğini kontrol edin.
3. Bir oyunda (ör. CS2, borderless) **CapFrameX** ile 60 sn'lik FPS/frame time ölçümü yapın, prototip **kapalıyken**.
4. Aynı sahnede prototip **açıkken** (`--window`) tekrar ölçün.
5. Klip dosyasını MPC-HC / VLC veya DaVinci'de açıp kontrol edin:
   - 5 ses izi var mı?
   - İzler senkron mu? (ör. oyunda ateş ederken Discord'da konuşun)
   - Görüntü akıcı mı?
6. Şunları paylaşın: `*.m0.txt` raporu ve iki CapFrameX sonucu (ortalama FPS, 1% low).

### Bilinen M0 sınırları (M1'de çözülecek)

- Geriye dönük halka (replay buffer) yok: prototip doğrudan dosyaya yazar.
- Ses izlerine isim (`Game`, `Chat`…) yazılmıyor, sırası: 1 Mix · 2 Game · 3 Chat · 4 Media · 5 Mic.
- Ses cihazı kaybolursa (Sonar yeniden başlarsa) iz sessiz devam eder, yeniden bağlanmaz.
- Exclusive fullscreen'de pencere yakalama siyah kalabilir; `--monitor` deneyin.
