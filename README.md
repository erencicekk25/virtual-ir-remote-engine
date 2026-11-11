# virtual-ir-remote-engine

[![CI](https://github.com/erencicekk25/virtual-ir-remote-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/erencicekk25/virtual-ir-remote-engine/actions/workflows/ci.yml)
![Language](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

Elinizdeki televizyon kumandasının bir tuşuna bastığınızda aslında ne olduğunu hiç merak ettiniz mi? Bu proje tam olarak o soruya cevap vermek için yazıldı. Gerçek bir kızılötesi (IR) LED'e ya da alıcıya ihtiyaç duymadan, **NEC protokolünü** bit bit ve mikrosaniye mikrosaniye taklit eden bir kumanda ile bu sinyalleri dinleyip çözen sanal bir televizyonu modern C++17 ile hayata geçiriyor.

Kumanda ve TV aynı makinede, iki ayrı bileşen olarak çalışıyor ve aralarındaki "hava" UDP üzerinden simüle ediliyor. Böylece hem protokolün mantığını adım adım izleyebiliyor hem de gürültü, yanlış adres, bozuk paket gibi gerçek hayatta karşılaşılan durumların alıcı tarafında nasıl ele alındığını görebiliyorsunuz.

---

## İçindekiler

1. [IR protokolleri nasıl çalışır?](#ir-protokolleri-nasıl-çalışır)
2. [NEC protokolü ve bit zamanlama katmanı](#nec-protokolü-ve-bit-zamanlama-katmanı)
3. [Simülasyonun mimarisi](#simülasyonun-mimarisi)
4. [Kaynak kod düzeni](#kaynak-kod-düzeni)
5. [Derleme ve çalıştırma](#derleme-ve-çalıştırma)
6. [Kullanım](#kullanım)
7. [Bellek güvenliği ve RAII](#bellek-güvenliği-ve-raii)
8. [Testler](#testler)
9. [Geliştirme fikirleri](#geliştirme-fikirleri)

---

## IR protokolleri nasıl çalışır?

Bir kumandanın ucundaki LED, insan gözünün göremediği ~940 nm dalga boyunda ışık yayar. Ancak ortamda güneş, ampuller ve diğer ısı kaynakları da sürekli kızılötesi ışık saçtığı için "LED yandı = 1, söndü = 0" gibi basit bir yaklaşım işe yaramaz. Bu yüzden IR haberleşmesi iki katmanlı bir hileye dayanır:

**1. Taşıyıcı frekans (carrier).** LED sabit yanmak yerine saniyede 38.000 kez yanıp söner (38 kHz). TV'deki alıcı modül (ör. TSOP serisi) yalnızca bu frekansta titreşen ışığa tepki verecek şekilde filtrelenmiştir; arka plandaki sabit ışığı tamamen yok sayar.

**2. Darbe kodlaması (pulse distance encoding).** Bilgi, taşıyıcının ne kadar süre *açık* (mark) ve ne kadar süre *kapalı* (space) kaldığıyla taşınır. Yani "1" ve "0", ışığın kendisiyle değil, **zamanlamayla** ifade edilir.

Dolayısıyla alıcı tarafında yapılan iş aslında bir kronometre okumaktır: gelen mark/space sürelerini ölçmek ve bunları protokolün tanımladığı kalıplarla karşılaştırmak. Bu projedeki TV tam olarak bunu yapıyor.

Piyasada NEC, RC-5, RC-6, Sony SIRC gibi pek çok protokol var. NEC, sade yapısı ve Asya menşeli cihazlarda çok yaygın kullanılması sayesinde öğrenmek için en uygun aday olduğundan simülasyon onun üzerine kuruldu.

---

## NEC protokolü ve bit zamanlama katmanı

### Çerçeve (frame) yapısı

Bir NEC çerçevesi 32 bitten oluşur ve dört bayta bölünür:

| Bayt | İçerik             | Amaç                                       |
|------|--------------------|--------------------------------------------|
| 0    | `address`          | Hangi cihaza gönderildiği                  |
| 1    | `~address`         | Adresin bit bit tersi (bütünlük kontrolü)  |
| 2    | `command`          | Basılan tuşun kodu                         |
| 3    | `~command`         | Komutun bit bit tersi (bütünlük kontrolü)  |

Ters baytlar basit ama etkili bir hata denetimi sağlar: her bayt kendi tersiyle eşleşmiyorsa çerçeve çöpe atılır. Bunun hoş bir yan etkisi de var: her çerçevede tam olarak 16 adet "1" ve 16 adet "0" bulunduğundan, **hangi tuşa basılırsa basılsın çerçevenin süresi hep aynıdır.** Testlerden biri tam olarak bunu doğruluyor.

Bitler havaya **en düşük anlamlı bitten (LSB) başlayarak** gönderilir. Örneğin `0x04` adresli TV'ye `Power (0x08)` komutu şu şekilde gider:

```
word = 0xF708FB04
bits (yayın sırasıyla) = 00100000 11011111 00010000 11101111
                         address   ~address  command   ~command
```

### Zamanlama tablosu

| Sinyal parçası            | Mark (taşıyıcı açık) | Space (taşıyıcı kapalı) |
|---------------------------|----------------------|-------------------------|
| Başlangıç (leader)        | 9000 µs              | 4500 µs                 |
| Mantıksal `0`             | 562 µs               | 562 µs                  |
| Mantıksal `1`             | 562 µs               | 1687 µs                 |
| Bitiş (trailing)          | 562 µs               | —                       |
| Tekrar kodu (repeat code) | 9000 µs + 2250 µs space + 562 µs mark | |

Bir tam çerçeve 2 (leader) + 64 (32 bit × mark/space) + 1 (trailing) = **67 darbeden** oluşur ve yaklaşık **67,5 ms** sürer.

### Tuşu basılı tutmak: tekrar kodları

Ses tuşunu basılı tuttuğunuzda kumanda aynı çerçeveyi defalarca göndermez. İlk çerçeveden sonra her 108 ms'de bir yalnızca 3 darbelik, kısacık bir **tekrar kodu** yollar. Alıcı da "az önce ne geldiyse onu bir kez daha uygula" der. Bu projede:

- Kumanda, `vol+ 5` gibi bir girdide önce tam çerçeveyi, ardından 108 ms arayla 5 tekrar kodunu gönderir.
- TV, tekrar kodunu yalnızca son geçerli çerçeveden sonraki **200 ms** içinde gelirse kabul eder; aksi halde "sahipsiz tekrar" olarak yok sayar.
- Yalnızca anlamlı olan komutlar tekrarlanabilir (ses ve kanal). `Power` ya da `Mute` tuşunu basılı tutmak TV'yi açıp kapatıp durmaz.

### Tolerans ve gürültü

Gerçek dünyada hiçbir sinyal tam 562 µs sürmez; LED'in tepki süresi, kumandadaki ucuz osilatör ve alıcının demodülasyon gecikmesi zamanlamaları kaydırır. Bu yüzden alıcı, her süreyi beklenen değerin **±%25'i** içinde kalıp kalmadığına bakarak yorumlar. Simülasyonda kumandaya `jitter` parametresiyle yapay gürültü eklenebiliyor:

- `%5–%20` jitter: sinyal bozulur ama TV yine de doğru çözer.
- `%60+` jitter: zamanlamalar tanınmaz hale gelir ve TV paketi `TimingError` ile reddeder.

---

## Simülasyonun mimarisi

```
┌──────────────────────────────┐                    ┌──────────────────────────────────┐
│        RemoteController      │                    │          VirtualTvDevice         │
│                              │                    │      (ayrı worker thread)        │
│  kullanıcı girdisi ("vol+")  │                    │                                  │
│            │                 │                    │   UDP alıcı (127.0.0.1:port)     │
│            ▼                 │                    │            │                     │
│  IrProtocolEncoder           │                    │            ▼                     │
│   • Command → NecFrame       │                    │   wire::deserialize              │
│   • NecFrame → 32-bit word   │                    │            │                     │
│   • word → PulseTrain (µs)   │                    │            ▼                     │
│            │                 │                    │   IrSignalParser                 │
│            ▼                 │                    │   • zamanlama toleransı          │
│  jitter (opsiyonel gürültü)  │                    │   • bit çözümü (LSB first)       │
│            │                 │    UDP datagram    │   • ~address / ~command kontrolü │
│            ▼                 │  ───────────────►  │            │                     │
│  wire::serialize ──► socket  │   "simüle hava"    │            ▼                     │
└──────────────────────────────┘                    │   adres filtresi + repeat penceresi
                                                    │            │                     │
                                                    │            ▼                     │
                                                    │   TvStateMachine                 │
                                                    │   STANDBY ⇄ ON, ses, kanal, kaynak│
                                                    └──────────────────────────────────┘
```

Sistem dört katmana ayrılıyor ve her katman bir öncekinden habersiz çalışabiliyor:

1. **Komut katmanı** – `ir::Command`: Kullanıcının bastığı mantıksal tuş (`Power`, `VolumeUp`, `Digit4` …).
2. **İkili çerçeve katmanı** – `ir::NecFrame` ve `packFrame / unpackFrame`: Adres ve komutu 32 bitlik kelimeye dönüştürür, ters baytları ekler ve doğrular.
3. **Zamanlama katmanı** – `ir::PulseTrain`: 32 biti mark/space sürelerinden oluşan bir diziye çevirir. Gerçek bir donanımda bu dizi doğrudan LED sürücüsüne verilirdi.
4. **Taşıma katmanı** – `ir::wire`: Darbe dizisini küçük bir ikili pakete sarar ve UDP üzerinden yollar. Paket formatı:

   | Ofset | Boyut | Alan                                 |
   |-------|-------|--------------------------------------|
   | 0     | 4     | Sihirli değer `"IRPT"`               |
   | 4     | 1     | Format sürümü (`1`)                  |
   | 5     | 1     | Rezerve                              |
   | 6     | 2     | Darbe sayısı (big-endian)            |
   | 8     | 4     | Taşıyıcı frekansı, Hz (big-endian)   |
   | 12    | 4·N   | Süreler, µs (big-endian)             |

Ağ üzerinden bitlerin kendisi değil, **ham zamanlamalar** gönderiliyor. Bu bilinçli bir tercih: böylece TV, gerçek bir IR alıcısı gibi davranmak zorunda kalıyor ve çözümleme (parsing) mantığının tamamı alıcı tarafında yaşıyor.

### TV durum makinesi

```
              Power                     
   ┌─────────┐ ─────────────► ┌────────────────────────────────────┐
   │ STANDBY │                │                 ON                 │
   │         │ ◄───────────── │  volume 0–100, mute, kanal 1–999,  │
   └─────────┘     Power      │  TV → HDMI1 → HDMI2 → AV kaynağı,  │
   (diğer tüm tuşlar           │  rakamlarla doğrudan kanal girişi  │
    yok sayılır)               └────────────────────────────────────┘
```

- **STANDBY** durumunda yalnızca `Power` kabul edilir.
- **ON** durumunda ses sınırlar içinde tutulur, sessizdeyken ses tuşuna basmak sesi geri açar, kanallar 999'dan 1'e döner.
- Rakam tuşları bir tampon biriktirir: `4`, `2`, `ok` → kanal 42. Üçüncü rakam girildiğinde onay beklemeden geçiş yapılır.

---

## Kaynak kod düzeni

```
virtual-ir-remote-engine/
├── CMakeLists.txt               Modern CMake (hedef tabanlı) yapılandırma
├── include/
│   ├── IrProtocolEncoder.hpp    Komut, çerçeve, zamanlama ve wire formatı
│   ├── VirtualTvDevice.hpp      Sinyal ayrıştırıcı, durum makinesi, sanal TV sunucusu
│   ├── RemoteController.hpp     Kumanda simülatörü
│   └── UdpSocket.hpp            Platformdan bağımsız RAII UDP soket sarmalayıcısı
├── src/
│   ├── IrProtocolEncoder.cpp
│   ├── VirtualTvDevice.cpp
│   ├── RemoteController.cpp
│   ├── UdpSocket.cpp            Winsock (Windows) / BSD soketleri (Linux, macOS)
│   └── main.cpp                 Etkileşimli CLI ve demo senaryosu
└── tests/
    └── EngineTests.cpp          Harici kütüphane gerektirmeyen birim + entegrasyon testleri
```

---

## Derleme ve çalıştırma

### Gereksinimler

- C++17 destekli bir derleyici (GCC 8+, Clang 7+, MSVC 2019+ veya MinGW-w64)
- CMake 3.16 veya üzeri
- (Önerilen) Ninja

Ek bir bağımlılık yok. Windows'ta `ws2_32` CMake tarafından otomatik bağlanır.

### Linux / macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/virtual_ir_remote
```

### Windows (MinGW-w64 veya MSVC)

```powershell
cmake -S . -B build -G Ninja
cmake --build build
.\build\virtual_ir_remote.exe
```

> **Not:** Proje klasörünün yolunda Türkçe karakter (ör. `Kişisel Projeler`) varsa `MinGW Makefiles` üreticisi kaynak dosyaları bulamayabiliyor. Bu durumda yukarıdaki gibi `-G Ninja` kullanmanız yeterli.

### CMake seçenekleri

| Seçenek                   | Varsayılan | Açıklama                                                  |
|---------------------------|------------|-----------------------------------------------------------|
| `VIRE_BUILD_TESTS`        | `ON`       | Test çalıştırılabilir dosyasını derler                    |
| `VIRE_ENABLE_SANITIZERS`  | `OFF`      | GCC/Clang ile AddressSanitizer + LeakSanitizer + UBSan     |

---

## Kullanım

### Komut satırı argümanları

```
virtual_ir_remote [--demo] [--jitter <oran>] [--port <udp-portu>]
```

- `--demo` : Hazır bir senaryoyu baştan sona oynatır; her şeyi tek seferde görmek için en hızlı yol.
- `--jitter` : Kumandanın eklediği zamanlama gürültüsü (varsayılan `0.05`, yani ±%5).
- `--port` : TV'nin dinleyeceği UDP portu. Verilmezse işletim sistemi boş bir port seçer.

### Etkileşimli mod

Program açıldığında bir istem (`>`) belirir. Yazabileceğiniz şeyler:

| Girdi                              | Etkisi                                                 |
|------------------------------------|--------------------------------------------------------|
| `power`, `mute`, `input`, `ok`     | İlgili tuşa bir kez basar                              |
| `vol+`, `vol-`, `ch+`, `ch-`       | Ses / kanal                                            |
| `0` … `9`                          | Rakam tuşları                                          |
| `vol+ 5`                           | Tuşu basılı tutar: 1 çerçeve + 5 tekrar kodu           |
| `status`                           | TV'nin anlık durumunu gösterir                         |
| `jitter 0.3`                       | Kumanda gürültüsünü değiştirir                         |
| `address 0x20`                     | Kumandanın hedef adresini değiştirir (TV `0x04`'tür)   |
| `help` / `quit`                    | Yardım / çıkış                                         |

### Örnek çıktı

```
> power
  [REMOTE] Power  addr=0x04 cmd=0x08 word=0xF708FB04
           bits(LSB first)=00100000 11011111 00010000 11101111
           pulses=67 airtime=68.33 ms bytes=280
  [TV]     Power: STANDBY -> ON, resuming channel 1  | power=ON volume=20 channel=1 input=TV

> vol+ 2
  [REMOTE] VolumeUp  addr=0x04 cmd=0x02 word=0xFD02FB04
           bits(LSB first)=00100000 11011111 01000000 10111111
           pulses=67 airtime=68.70 ms bytes=280
  [TV]     VolumeUp: volume 21  | power=ON volume=21 channel=1 input=TV
  [REMOTE] repeat code  pulses=3 airtime=12.09 ms bytes=24
  [TV]     (held) volume 22  | power=ON volume=22 channel=1 input=TV
  [REMOTE] repeat code  pulses=3 airtime=11.71 ms bytes=24
  [TV]     (held) volume 23  | power=ON volume=23 channel=1 input=TV

> address 0x20
  remote targets address 0x20

> power
  [REMOTE] Power  addr=0x20 cmd=0x08 word=0xF708DF20
  ...
  [TV]     frame for address 0x20 ignored (mine is 0x04)

> jitter 0.70
> ch-
  ...
  [TV]     decode failed (TimingError): leader space 7291 us is not ~4500 us
```

Jitter rastgele üretildiği için süreler (ve yüksek gürültüde hatanın tam yeri) her çalıştırmada biraz farklı olacaktır.

---

## Bellek güvenliği ve RAII

Projede tek bir satır bile manuel `new` / `delete` ya da `malloc` / `free` yok. Kaynakların tamamı sahiplik kurallarıyla yönetiliyor:

- **`net::UdpSocket`** yalnızca taşınabilir (move-only) bir sınıf. Soket tanıtıcısı yapıcıda açılıp yıkıcıda kapanıyor; kopyalama yasaklı olduğu için aynı soketin iki kez kapatılması mümkün değil.
- **Winsock başlatma** (`WSAStartup` / `WSACleanup`), fonksiyon içi statik bir RAII nesnesinde tutuluyor; ilk soket oluşturulduğunda bir kez başlıyor, program kapanırken kendiliğinden temizleniyor.
- **`tv::VirtualTvDevice`** kendi worker thread'ine sahip. Yıkıcısı `stop()` çağırıp thread'i `join` ediyor, yani nesne kapsam dışına çıktığında arkasında asılı bir thread ya da açık bir soket kalmıyor. Soketin 100 ms'lik okuma zaman aşımı sayesinde kapanma her zaman hızlı ve deterministik.
- Paylaşılan durum (`TvStateMachine`, işlenen paket sayacı) bir `std::mutex` ile korunuyor; okuma tarafı `snapshot()` ile kopya alıyor, böylece kilit dışarı sızmıyor.
- Worker thread içinde oluşabilecek her istisna yakalanıyor; bir dinleyici (listener) hata fırlatsa bile alıcı döngüsü çökmüyor.
- Tüm tamponlar `std::vector`, `std::string` ve `std::optional` ile yönetiliyor.

Linux veya macOS'ta sızıntı kontrolünü kendiniz de yapabilirsiniz:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DVIRE_ENABLE_SANITIZERS=ON
cmake --build build-asan
./build-asan/ir_engine_tests
./build-asan/virtual_ir_remote --demo
```

Valgrind tercih ederseniz: `valgrind --leak-check=full ./build/virtual_ir_remote --demo`

---

## Testler

```bash
ctest --test-dir build --output-on-failure
# ya da doğrudan:
./build/ir_engine_tests
```

Test paketi şunları kapsıyor:

- Çerçeve paketleme / açma ve ters bayt doğrulaması
- Her komut için sabit çerçeve süresi (16 adet "1" kuralı)
- Kodlama → çözme gidiş-dönüşü (tüm komutlar ve tekrar kodu)
- Bozuk başlangıç darbesi, eksik darbe, yanlış taşıyıcı ve tek bit hatasının reddedilmesi
- ±%20 sapmaya rağmen doğru çözümleme
- Wire formatının serileştirilmesi ve bozuk paketlerin reddi
- Durum makinesinin tüm geçişleri ve ses sınırları
- Arka arkaya oluşturulup yok edilen cihazlarla RAII temizliği
- Gerçek UDP soketleri üzerinden uçtan uca senaryo (açma, basılı tutma, yanlış adres, gürültü)

---

## Geliştirme fikirleri

- RC-5 / RC-6 (Manchester kodlama) ve Sony SIRC protokollerini ayrı encoder'lar olarak eklemek
- Genişletilmiş NEC (16 bit adres) desteği
- Darbe dizisini bir WAV dosyasına yazarak sinyali osiloskop benzeri bir araçta görselleştirmek
- Aynı "havayı" paylaşan birden fazla cihaz (multicast) ile çakışma senaryoları
- Gerçek bir donanım için çıktı: darbe dizisini bir mikrodenetleyiciye (ESP32, Arduino) seri porttan göndermek
