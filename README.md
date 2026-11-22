# Waveline

SteelSeries Moments ve Sonar'ın hafif, açık kaynak karşılıkları.

| Bileşen | Karşılığı | Durum |
|---|---|---|
| **Waveline Clip** | Moments: geriye dönük kayıt + manuel kayıt, 5 ses izi | M2 (konsol) |
| **Waveline Mix** | Sonar: Game / Chat / Media / Mic katmanları, Personal + Stream Mix | Planlandı |

Analiz, mimari ve kararlar: [ANALIZ_VE_PLAN.md](ANALIZ_VE_PLAN.md)

---

## Gereksinimler

- Windows 11, NVIDIA GPU (NVENC)
- Visual Studio 2026 (veya 2022) ve şu bileşenler:
  - **Desktop development with C++** iş yükü
  - **Windows 11 SDK (10.0.26100 veya daha yeni)** ← C++/WinRT başlıkları bununla gelir

## Derleme

Visual Studio'da `Waveline.sln` dosyasını açıp **Release | x64** derleyin, ya da:

```powershell
& "C:\Library\Apps\Visual Studio\MSBuild\Current\Bin\MSBuild.exe" Waveline.sln /p:Configuration=Release /p:Platform=x64 /m
```

Çıktılar `build\x64\Release\` altında:
- `WavelineClip.exe` — otomatik algılama + geriye dönük kayıt
- `WavelineClipProto.exe` — M0 ölçüm prototipi

Klasör yapısı:
- `src\clip-core\` — ortak motor: yakalama, GPU dönüşümü, NVENC, ses, halka, MP4
- `src\clip\` — WavelineClip uygulaması
- `src\clip-proto\` — M0 prototipi

---

## `WavelineClip` (M2: otomatik algılama + geriye dönük kayıt)

Arka planda bekler, oyun açılınca kendiliğinden kayda başlar. Son N dakikayı RAM'de sıkıştırılmış paketler olarak tutar. Kısayola basınca klibi **yeniden kodlamadan**, yaklaşık 0.1–0.4 saniyede MP4 olarak yazar.

```powershell
& "C:\Library\Projects\Visual Studio\Waveline\build\x64\Release\WavelineClip.exe" --minutes 2 --dir E:\Records
```
Programı açık bırakıp oyunu başlatın. Klip için **Ctrl+Alt+F10**, çıkış için konsolda Ctrl+C.

**Nasıl çalışır**
- **Oyun algılama:** Ön plandaki pencere monitörü başlık çubuğu olmadan kaplıyorsa (borderless/fullscreen) ya da Windows exclusive fullscreen bildiriyorsa, ve 1.5 sn böyle kalırsa oyun sayılır.
- **Hariç tutulanlar:** Tarayıcılar, medya oynatıcılar, Discord, OBS, Explorer vb. tam ekran olsa da kaydedilmez. Liste için `--list`.
- **Kayıt süresi:** Oyun penceresi kapanana kadar kaydedilir; alt-tab yapınca kayıt durmaz.
- **Masaüstü modu:** `--desktop` ile açılır; oyun yokken seçili monitör kaydedilir. Oyun açılınca otomatik oyuna, oyun kapanınca masaüstüne geçer. Parsec gibi sanal ekranlar seçilmez.
- **Boşta yük:** Oyun yokken (ve masaüstü modu kapalıyken) yakalama ve encoder tamamen kapalıdır. Ölçülen: ~17 MB RAM, CPU ~%0.02.
- **Anti-cheat:** Oyun algılama oyuna dokunmaz. Pencere olayları süreç dışından dinlenir, exe adı en düşük yetkiyle okunur.

| Seçenek | Açıklama |
|---|---|
| `--minutes M` | Geriye dönük süre, 0.5–10 dk (varsayılan 1) |
| `--dir KLASÖR` | Kliplerin klasörü (varsayılan: Videolar\Waveline) |
| `--hotkey TUŞLAR` | Ör. `ctrl+alt+f10`, `alt+f9`, `ctrl+shift+s` |
| `--desktop` | Oyun yokken masaüstünü kaydet |
| `--monitor N` | Masaüstü modunun monitörü (`--list` ile numaralara bakın) |
| `--include a,b` | Bu exe'ler pencereli modda da oyun sayılır (ör. `--include TravellersRest`) |
| `--exclude a,b` | Bu exe'ler hiç kaydedilmez (varsayılan listeye eklenir) |
| `--list` | Monitörleri ve hariç listesini göster |
| `--fps`, `--bitrate`, `--codec`, `--no-cursor` | Kalite ayarları |
| `--no-sound` | Klip alındı sesini kapat |
| `--auto-save SN`, `--exit-after SN` | (test yardımcıları) |

Klip özellikleri:
- **Dosya adı:** `oyunexe__YYYY-MM-DD__HH-mm-ss.mp4`; masaüstü için `Masaustu__...`.
- **İzler:** Ses izleri isimli: `Mix`, `Game`, `Chat`, `Media`, `Mic`.
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
