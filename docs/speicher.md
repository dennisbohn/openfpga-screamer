# Speicher (Schritt 2: SDRAM und Speichertest)

**Seit 2026-10-26 sind SRAM und PSRAM ausgebaut.** Nur der Speichertest hat sie benutzt; die
Sounds kommen in den SDRAM, der Audiopuffer in den Block-RAM. Ihre Pins liegen fest auf
inaktiv (`core_top.v`). Wie die Controller gebaut waren, steht unten kurz und vollständig
in der Git-Historie (bis Commit `837a781`).

Stand 2026-10-25: gebaut und in der Simulation geprüft, der Lauf auf dem Pocket steht noch
aus. Der Entwurf ging vorher durch eine Prüfung aus fünf Blickwinkeln (39 Befunde,
eingearbeitet). Grundlage sind die Datenblätter und eine Recherche in 18 quelloffenen
Pocket-Cores; zwei Prüfer haben Testprojekte in Quartus gebaut. Der Vertrag zwischen Hardware
und Firmware steht weiter in `docs/soc.md`; hier stehen der Aufbau der Controller und die
Gründe dafür.

## Ziel

- **SDRAM** (64 MB): Hauptspeicher der CPU für Programm und Daten, über I-Cache und D-Cache,
  dazu ein ungecachtes Fenster für Tests und ein Schreibpfad von der APF-Bridge, über den der
  Pocket Data-Slots direkt ins SDRAM lädt.
- **SRAM** (256 KB) und **PSRAM** `cram0`, `cram1` (je 16 MB): nicht benutzt (siehe oben).
- **Speichertest** als Firmware: das SDRAM mit Mustern beschreiben und zurücklesen,
  das SDRAM-Timing auf dem Pocket vermessen, Ergebnis auf dem Bildschirm, in `debug.txt` und
  für die Simulation in `SYS_SIM`.

## Die Chips

| | SDRAM | SRAM | PSRAM |
|---|---|---|---|
| Teil | AS4C32M16MSA-6BIN, Mobile SDRAM | AS6C2016-55BIN | AS1C8M16PL-70BIN, zwei Dies |
| Größe | 4 Bänke × 8192 Zeilen × 1024 Spalten × 16 Bit | 128K × 16 Bit | 2 × 4M × 16 Bit |
| Spannung, IO | 1,8 V, 4 mA, Bank 7A | 3,3 V LVCMOS, 2 mA | 1,8 V, 4 mA |
| Schnittstelle | synchron, `dram_clk`, kein CS | asynchron, kein CE | asynchron, Adresse und Daten gemultiplext |

SDRAM-Timing bei 74,25 MHz (T = 13,468 ns), aus dem Datenblatt in Takten:

| CL | tRCD | tRP | tRAS | tRC | tRRD | tWR | tRFC | tMRD | Refresh |
|---|---|---|---|---|---|---|---|---|---|
| 2 (bis 83 MHz erlaubt) | 2 | 2 | 4 | 5 | 1 | 2 | 11 (138 ns, sicherer als die 80 ns der Tabelle) | 2 | 8192 je 64 ms: alle 7,8 µs = 580 Takte |

## Takt und IO-Timing

Alles läuft weiter auf `clk_74a`, auch der SDRAM-Controller: kein Taktübergang zwischen CPU,
Controller und Bridge. Ein höherer Systemtakt ist ein eigener späterer Schritt.

`dram_clk` kommt aus einer **zweiten PLL** `pll_sdram` (Referenz `clk_74a`, Ausgang
74,25 MHz, ganzzahlig, erzeugt von `tools/gen_pll_sdram.sh` mit Intels `ip-generate`) über ein
DDIO-Register (`altddio_out`, high = 1, low = 0). Verlangt sind 10.000 ps; Quartus wählt VCO
742,5 MHz (M = 10, C = 10) und liefert **9932 ps** (265,5°). Ein Schritt der dynamischen
Phasenverschiebung ist ein Achtel der VCO-Periode, **168 ps**; 80 Schritte sind eine Periode.
Die Template-PLL kann 74,25 MHz neben 12,288 MHz nicht erzeugen.

Gemessen in den Testprojekten der Prüfer (vier Ecken, Platine 0–0,3 ns je Leitung,
Ausgangsverzögerung im IO voll ausgenutzt; dort noch mit anderem VCO):

| φ | Ausgang Setup | Ausgang Hold | Lesen Setup | Lesen Hold |
|---|---|---|---|---|
| 10.000 ps | 4,36 ns | 2,12 ns | 1,29 ns | 1,58 ns |
| 11.000 ps | 5,36 ns | 1,12 ns | 1,69 ns | 1,99 ns |
| 180° aus `clk_74a` (DDIO) | – | – | – | 0,68 ns (knappste Prüfung) |

Der echte Abstand liegt also bei etwa 1,3–1,5 ns, in der Größe dessen, was das Modell nicht
kennt (PLL-Jitter am IO bis 0,88 ns, Platine, tOH bei kleiner Last).

Gemessen im Core selbst (Quartus 18.1.1, φ = 9932 ps, Constraints wie unten, mit den
0,3 ns Zusatz-Unsicherheit), Abstand in ns, Bericht von `tools/sta_sdram.tcl`:

| Ecke | Ausgang Setup | Ausgang Hold | Lesen Setup | Lesen Hold |
|---|---|---|---|---|
| Slow 85 °C | 3,134 | 2,656 | **0,841** | 4,415 |
| Slow 0 °C | 3,235 | 2,678 | 1,432 | 3,990 |
| Fast 85 °C | 4,802 | 2,862 | 5,658 | 1,563 |
| Fast 0 °C | 4,920 | 2,795 | 6,230 | **1,060** |

Das Lese-Fenster ist also in der langsamen Ecke beim Setup knapp und in der schnellen beim
Hold; die Phasenmessung auf dem Pocket zeigt, wo es wirklich liegt. Deshalb:

- **Phase zur Laufzeit verstellbar:** Die SDRAM-PLL hat ihre Anschlüsse für dynamische
  Phasenverschiebung (`phase_en`, `updn`, `cntsel`, `phase_done`, `scanclk` = `clk_74a`);
  `pll_dps.v` macht daraus einen Schritt pro Auftrag aus `MEM_PHASE`. Der Speichertest fährt
  die Phase in 168-ps-Schritten über eine ganze Periode, initialisiert das SDRAM bei jedem
  Schritt kurz neu, prüft mit harten Mustern, schreibt das Fenster ins Protokoll und stellt die
  Mitte des Fensters ein. Der Bitstream startet mit 9932 ps.
- **Die Suche zerstört Speicherinhalt:** In den schlechten Phasen übernimmt der SDRAM-Chip
  Befehle und Adressen offenbar falsch. Eine Prüfsumme je MB vor und nach der Suche zeigte
  15 von 63 MB verändert, nicht nur die 4 KB des Tests (2026-10-26). Vorher geladene Daten
  waren betroffen: einmal eine leere Strecke, einmal ein Absturz an einem kaputten Verweis.
  Deshalb lädt der Pocket nichts mehr vorab ins SDRAM (`deferload`), der Lader fordert
  `program.bin` erst nach der Suche und den Tests an (`docs/soc.md`).
- **Constraints genau so** (`core_constraints.sdc`): `create_generated_clock -name
  dram_clk_pin -source [PLL-Ausgang divclk] [get_ports dram_clk]` (ohne `-invert`; bei der
  PLL mit Phasenverschiebung heißt die Quelle `…|cyclonev_pll|counter[0].output_counter|divclk`);
  `set_output_delay -clock dram_clk_pin -max 2.3 -min -1.3` auf allen `dram_*`-Ausgängen und
  `dram_dq`; `set_input_delay -clock dram_clk_pin -max 6.6 -min 2.5` auf `dram_dq`;
  `set_multicycle_path -setup -end 2` von `dram_clk_pin` nach `clk_74a` und **kein**
  Hold-Multicycle (mit `-hold 1` oder `-setup 3` wird die Lese-Hold-Prüfung bedeutungslos, das
  Testprojekt zeigte dann 14 ns statt echter 0,6 ns); `set_clock_uncertainty -add 0.3` für
  `clk_74a` ↔ `dram_clk_pin`. `clk_74a`, der PLL-Ausgang und `dram_clk_pin` dürfen **nie** in
  getrennte asynchrone Gruppen (sonst verschwinden alle SDRAM-IO-Prüfungen ohne Meldung).
- **Build-Prüfung:** `tools/build.sh` lässt nach der Timing-Analyse die vier Übergänge
  `clk_74a` ↔ `dram_clk_pin` (Setup und Hold, alle Ecken) berichten und bricht ab, wenn einer
  0 Pfade hat, unter 0,5 ns liegt oder der Lese-Hold über 5 ns liegt (Zeichen eines falschen
  Multicycles).
- **IO-Register:** `FAST_OUTPUT_REGISTER` auf allen Ausgängen des SDRAM,
  `FAST_OUTPUT_ENABLE_REGISTER` und `FAST_INPUT_REGISTER` auf den Datenleitungen;
  `tools/check_budget.py` prüft im Fitter-Bericht, dass die Register in den IO-Zellen liegen.

Leselatenz: Das SDRAM gibt ein Datenwort tAC nach seiner Flanke r+CL−1 aus und hält es bis tOH
nach Flanke r+CL; der Controller übernimmt es mit der Flanke **READ + CL + 1 = READ + 3**
(Parameter `READ_LATENCY` im Controller, im SDC-Kommentar beim Multicycle genannt: die
Setup-Vielfachheit ist READ_LATENCY − CL + 1 = 2). Für CL3 als Rückfall bliebe sie 2.

## SDRAM-Controller (`sdram.v`)

### Betriebsart

- Mode Register `0x221`: CL2, Burstlänge 2, sequentiell, Einzelschreiben (M9 = 1).
  Extended Mode Register (BA = 10): volles Feld, Treiberstärke `010` (25 %) wie Analogues
  eigener Controller und die Cores, die damit bei 96–100 MHz laufen (Parameter).
- Adressen: Byte-Adresse [25:0] → Bank = [12:11], Zeile = [25:13], Spalte = [10:1]. Ein
  **READ** geht immer an die gerade Spalte {[10:2], 0} und liefert das ganze 32-Bit-Wort:
  das Halbwort der Flanke READ+3 nach [15:0], das der Flanke READ+4 nach [31:16]; die CPU wählt
  die Bytes selbst, Adressbits [1:0] und Maske werden beim Lesen ignoriert. Ein **WRITE**
  schreibt ein Halbwort an Spalte {[10:2], h} mit DQM = ~Maske[2h+1:2h]; Halbwörter ohne
  Maskenbits fallen weg. Eine Cache-Zeile (32 Byte = 16 Spalten) überschreitet nie eine Zeile.
- **DQM:** 11 während der Initialisierung, beim WRITE die invertierte Maske, sonst 00 (also
  bei jedem READ und danach).
- **Offene Zeilen:** je Bank bleibt die zuletzt benutzte Zeile offen; Treffer sofort, andere
  Zeile: PRECHARGE, tRP, ACTIVATE, tRCD; Bank zu: ACTIVATE, tRCD.
- **Abstände** (je Bank gezählt, über Auftragsgrenzen, beim Refresh und beim Zeilenwechsel):
  PRECHARGE frühestens bei max(ACTIVATE + tRAS, letztes WRITE + tWR, letztes READ + 2);
  ACTIVATE frühestens bei max(PRECHARGE + tRP, ACTIVATE derselben Bank + tRC, ACTIVATE
  irgendeiner Bank + tRRD); nach jedem READ zwei Takte lang kein READ, WRITE, PRECHARGE oder
  BURST TERMINATE (sonst schneidet das SDRAM das zweite Halbwort ab), praktisch: jeder READ
  belegt zwei Takte; **WRITE frühestens 6 Takte nach dem letzten READ** (das SDRAM treibt
  dank φ von etwa READ+1,74 T bis READ+4,2 T plus tHZ).

### Ablauf

- **Einschalten:** Alle `dram_*`-Register haben feste Startwerte (NOP, CKE = 0, DQM = 11,
  Datenausgang aus). Erst wenn die SDRAM-PLL eingerastet ist (synchronisiert und 1024 Takte
  stabil): CKE = 1, 16.384 Takte NOP (220 µs > 200 µs), PRECHARGE ALL, 2 × AUTO REFRESH, MRS,
  EMRS. **Nicht an `reset_n` gekoppelt:** Der Pocket lädt Data-Slots, während er den Core im
  Reset hält; auch dann muss das SDRAM laufen und aufgefrischt werden. Verliert die PLL den
  Lock, gilt das SDRAM als nicht bereit, CKE geht auf 0, und nach dem Wiedereinrasten läuft die
  ganze Initialisierung neu.
- **Kurze Neuinitialisierung** (`MEM_PHASE` Bit 8, nach jedem Phasenschritt): Takt und CKE
  laufen weiter; sobald der laufende Auftrag und seine Daten durch sind, PRECHARGE ALL,
  2 × AUTO REFRESH, MRS, EMRS. Neue Aufträge warten so lange, statt Fehlerwörter zu bekommen,
  und der Inhalt bleibt erhalten. (Die erste Fassung schaltete CKE ab und lief die ganze
  Initialisierung mit 220 µs ohne Refresh; das Modell meldete das, der Inhalt wäre verloren.)
- **Refresh:** alle 512 Takte (6,9 µs) fällig, zwischen zwei Aufträgen ausgeführt:
  PRECHARGE ALL (mit den Abständen oben), AUTO REFRESH, tRFC. Ein Auftrag dauert höchstens etwa
  28 Takte, der Abstand bleibt unter 580. Ein 32-Bit-Zähler zählt jedes AUTO REFRESH
  (`MEM_REFRESH`), damit die Firmware auf dem Pocket die Rate prüfen kann.
- **Lesen:** pro 32-Bit-Wort ein READ; eine Cache-Zeile sind 8 READs im Abstand von 2 Takten,
  die 16 Halbwörter kommen lückenlos. Jeder READ schiebt eine Marke (Port, Epoche, letztes
  Wort) durch eine Kette der Länge `READ_LATENCY`; das Wort ist einen Takt nach dem ersten
  Halbwort fertig und geht aus einem Register an den Port. Der nächste Auftrag darf beginnen,
  während die Daten des vorigen noch unterwegs sind (die Abstände oben gelten trotzdem).
- **Schreiben:** pro Halbwort mit Maskenbits ein WRITE, Daten am IO-Register.
- **Nicht bereit:** Solange das SDRAM nicht initialisiert ist, beantwortet der Controller
  Leseaufträge sofort mit der richtigen Zahl Fehlerwörter und verwirft Schreibaufträge. Die
  CPU hat keine Zeitgrenzen; ein unbeantworteter Auftrag würde sie anhalten.
- **Pins:** getrennte `dq_o`/`dq_oe`/`dq_i`, der Tristate sitzt in `core_top`.

### Ports, Arbiter und Reihenfolge (`sdram_port.v`)

Ein eigenes Modul zwischen CPU, Bridge und Controller, damit der Zufallstest es direkt treiben
kann (`soc.v` bindet es unverändert ein):

- **Ein Befehlsregister je CPU-Port** (iBus 1 Eintrag, dBus 1 Eintrag), bei der Annahme mit
  dem schon dekodierten Ziel geladen; `cmd_ready` = Register leer, nur aus Registern. Beide
  Ports können im selben Takt annehmen.
- **Bridge-Schreibpuffer** (4 Einträge): Der Pocket liefert höchstens ein Wort alle 88 Takte;
  ein Überlauf wird als Fehlerbit festgehalten.
- **Zeilen-Port** für Rasterizer (Texturen), Geometrie-Einheit (Meshes) und Blitter (Bilder),
  in `soc.v` zwischen ihnen geteilt (Rasterizer vor Geometrie vor Blitter): ein Befehlsregister
  und bis zu **zwei Zeilen im Controller**. Der Controller beginnt die nächste Zeile, während
  die Wörter der vorigen noch kommen; dicht folgende Zeilen kommen so im Abstand der 8 READs
  statt der ganzen Latenz. Die Wörter kommen in der Reihenfolge der Aufträge zurück, eine
  FIFO mit zwei Einträgen in `soc.v` merkt sich, wem sie gehören (2026-10-28: am Start mit allen
  Autos wartet der Rasterizer 188.000 statt 299.000 Takte auf Texturen).
- An jeder Auftragsgrenze wählt der Arbiter: Bridge-Puffer vor dBus-Register vor
  Zeilen-Port vor iBus-Register.
- **Reihenfolge-Regel:** Schreibzugriffe einer Quelle laufen in der Reihenfolge ihrer Annahme.
  Jeder Lesezugriff liefert Daten, die alle vorher angenommenen Schreibzugriffe enthalten; ein
  iBus-Lesen darf auch spätere schon enthalten. (Ein Lesen nach einem Schreiben im selben Port
  wartet dadurch automatisch, weil die CPU erst nach der Annahme weitermacht und der Arbiter
  ein Register nicht überholen lässt.)
- **CPU-Reset:** noch nicht begonnene CPU-Leseaufträge werden verworfen; Wörter von Aufträgen,
  die schon im Controller laufen, verwirft eine 1-Bit-Epoche in der Marke (bei jedem
  CPU-Reset umgeschaltet), egal wie kurz der Reset ist. Angenommene CPU-Schreibzugriffe laufen
  noch durch, nie nach dem Reset. Arbiter und Controller werden von `cpu_reset` nicht
  zurückgesetzt, nur die CPU-Register.
- **„Alle Schreibzugriffe erledigt"** (`MEM_STATUS`): NOR über jede Stufe, die einen
  angenommenen Schreibzugriff halten kann (dBus-Register, Controller bis zum letzten WRITE).
  Ohne Zähler. Ein `lw MEM_STATUS` direkt nach einem `sw` liest 0, solange das Schreiben noch
  läuft (ins SDRAM mit Zeilenwechsel 7 Takte nach der Annahme). Gebraucht wird es, wenn eine
  Einheit Daten liest, die die CPU gerade in den SDRAM geschrieben hat, denn Schreibzugriffe
  an verschiedene Ziele können sich überholen.

### Adressdekodierung

| Bereich | Bedingung | iBus | dBus |
|---|---|---|---|
| Boot-RAM (32 KB) | [31:15] = 0 | ja | wie bisher |
| SDRAM | [31:26] = `000100` (0x1000_0000) | Zeilen | Zeilen, Schreiben |
| SDRAM ungecacht | [31:26] = `111101` (0xF400_0000) | Busfehler | Einzel-Lesen, Schreiben |
| sonst | | Busfehler | wie in `docs/soc.md` |

„Busfehler" heißt beim iBus: `soc.v` antwortet selbst mit 8 Wörtern, Fehlerbit auf den Wörtern
0–6 (wie in Schritt 1); solche Zeilen erreichen den Arbiter nicht.

### Bridge und Data-Slots

Bridge-Adressen `0x1000_0000`–`0x13FF_FFFF` schreiben ins SDRAM, Byte-Reihenfolge wie beim
Boot-RAM gedreht. Lesen über die Bridge gibt es noch nicht. Der Pocket fragt vor jedem Slot
mit „Data slot request write" (0x0082) an; die Quittung kommt, sobald das SDRAM bereit ist,
spätestens aber nach 2²¹ Takten (28 ms) Warten, damit ein SDRAM-Fehler den Start nicht
blockiert (dann werden Bridge-Schreibzugriffe ins SDRAM verworfen und ein Fehlerbit gesetzt).

Ein Zähler `MEM_BRIDGE_WORDS` zählt die Bridge-Wörter ins SDRAM; er und das Überlaufbit werden
beim Einschalten und an der fallenden Flanke von `reset_n` (Reset Enter) gelöscht. So weiß die
Firmware, ob der Test-Slot in diesem Lauf geladen wurde: Beim Neuladen der Firmware aus dem
Menü schreibt der Pocket nur den neu geladenen Slot.

Der Test-Slot (`core/data.json`, nach Slot 0 und 1):
`{"name": "Firmware SDRAM", "id": 2, "required": false, "parameters": "0x02",
"filename": "firmware.bin", "extensions": ["bin"], "size_maximum": 32768,
"address": "0x10000000"}` – Bit 1, weil die Datei im Core-Ordner liegt; kein nonvolatile,
kein deferload.

## SRAM und PSRAM (ausgebaut)

Bis 2026-10-26 gab es einen SRAM-Controller (`sram.v`, ein Halbwort in 7 Takten, alle Pins
aus IO-Registern) und für `cram0` agg23s asynchronen Controller (`ip/psram.sv` aus
[agg23/analogue-pocket-utils](https://github.com/agg23/analogue-pocket-utils), MIT, in
`psram32.v`; 8 Takte pro Halbwort), beide nur für den Speichertest. Zusammen etwa 100 ALMs.
Falls ein PSRAM wieder gebraucht wird, gilt aus dem Bau und der Prüfung auf dem Pocket:

- **Konfiguration nicht voraussetzen.** BCR/RCR überleben das Laden eines anderen Cores,
  nur Ausschalten setzt sie zurück. Asynchrone Zugriffe mit stehendem `cram_clk`
  funktionieren in jedem Modus; ein laufender Takt machte bei einem anderen Core aus
  asynchronen Schreibzugriffen Bursts.
- **Software-Access-Falle:** Zweimal Lesen, dann Schreiben an Halbwort `3FFFFFh` eines Dies
  ändert die Konfigurationsregister (`RCR[4] = 0` hieße Deep Power-Down); das Wort meiden.
- **IO-Timing festmachen:** alle `cram`-Ausgänge in IO-Register (`FAST_OUTPUT_REGISTER`, für
  `cram_dq` auch `FAST_OUTPUT_ENABLE_REGISTER` und `FAST_INPUT_REGISTER`).
- **Startwerte der Steuerleitungen:** Initialwerte in Port-Deklarationen übernimmt Quartus
  18.1 womöglich nicht; dann `POWER_UP_LEVEL HIGH` in der `.qsf`.

## Speicherkarte und Register (Ergänzung zu `docs/soc.md`)

| CPU-Adresse | Größe | Inhalt | Cache |
|---|---|---|---|
| `0x1000_0000` | 64 MB | SDRAM | I und D |
| `0xF400_0000` | 64 MB | SDRAM, dasselbe ungecacht | nein (Code: Busfehler) |

| Adresse | Name | | Bedeutung |
|---|---|---|---|
| `0xF000_0008` | `SYS_FLAGS` | R | Bit 0: Simulation (nur das Simulations-Top setzt es) |
| `0xF000_000C` | `MEM_STATUS` | R | Bit 0 SDRAM-PLL eingerastet, Bit 1 SDRAM bereit, Bit 2 Bridge-Puffer übergelaufen, Bit 3 Bridge-Schreiben verworfen (nicht bereit), Bit 4 alle Schreibzugriffe erledigt |
| `0xF000_0010` | `MEM_BRIDGE_WORDS` | R | Bridge-Wörter ins SDRAM seit Reset Enter |
| `0xF000_0014` | `MEM_REFRESH` | R | Zahl der AUTO REFRESH seit dem Einschalten |
| `0xF000_0018` | `MEM_PHASE` | R/W | Schreiben: Bit 0 = `dram_clk` um einen Schritt (168 ps) verschieben, Bit 1 = später (sonst früher), Bit 8 = SDRAM kurz neu initialisieren; Lesen: Bit 0 Verschiebung läuft, Bit 1 SDRAM bereit, Bit 31..16 Schritte seit dem Start (vorzeichenbehaftet; 80 = eine Periode) |

## Speichertest (Firmware)

`firmware/memtest.c`: ein Testablauf für einen Speicher, beschrieben durch (Basis,
ungecachte Basis, Größe, Die-Größe); bis zum Ausbau liefen SRAM und `cram0` mit. Fehler mit Adresse, Soll, Ist und zweimal ungecacht
nachgelesen (dreimal derselbe falsche Wert deutet auf Schreiben oder Adresse, wechselnde Werte
auf Lese-Timing); höchstens acht Fehlerzeilen je Speicher, danach wird nur gezählt. Nach
jedem Test eine Zeile mit „ok" oder „FAIL, errors n" und den Takten. Vor jedem Test und alle
64 KB darin stehen Test und Adresse auf dem Bildschirm und in der Kopfzeile des Protokolls,
damit ein Hängenbleiben eine Stelle zeigt. Jeder Test setzt bei Erfolg ein Bit in `SYS_SIM`
(Bit 3..12, Liste in `tools/sim_faults.sh`).

SDRAM, in dieser Reihenfolge:

1. **Bereitschaft:** bis 0,5 s auf `MEM_STATUS` Bit 1 warten, `SYS_FLAGS`, `MEM_STATUS` und
   `MEM_BRIDGE_WORDS` ins Protokoll; ist das SDRAM nicht bereit, werden die SDRAM-Tests
   übersprungen.
2. **program.bin:** läuft jetzt als letzter SDRAM-Test: vom Pocket angefordert (Kopf, dann
   die Datei), Kopf, Größe und CRC-32 geprüft, dazu die Bits „übergelaufen" und „verworfen".
3. **Phasen-Fenster:** 80 Schritte zu 168 ps (Simulation: 4), bei jedem kurz neu
   initialisieren und prüfen: 4 KB Pseudozufall über Cache-Zeilen (Simulation: 512 Byte) und 64
   Byte Umschaltmuster ungecacht. Die längste Folge guter Schritte (auf dem Pocket rund um den
   Kreis) ins Protokoll als Karte (`#` gut, `.` schlecht), Anfang und Länge; Mitte einstellen.
   Weniger als 8 gute Schritte (Simulation: 1) zählen als Fehler.
4. **Datenbus:** wandernde Eins und Null auf einem Wort (ungecacht).
5. **Byte-Lanes:** je Teil-Store (sb an 0–3, sh an 0 und 2) ein frisches Hintergrundwort
   schreiben, genau einen Store mit anderem Wert, ungecacht zurücklesen und mit dem Hintergrund
   vergleichen, in dem nur die Ziel-Bytes neu sind; dann ein bekanntes Wort mit lb, lbu, lh,
   lhu an jedem Offset lesen (Vorzeichen geprüft).
6. **Adressbus:** eigene Werte an `0` und allen Zweierpotenzen, erst alle schreiben, dann alle
   ungecacht zurücklesen.
7. **Muster über den ganzen Speicher** (Simulation: erstes und letztes Fenster von 2 KB),
   D-Cache vor jedem Prüfdurchgang leeren (`0x0000500F`), über Cache-Zeilen: Adresse als Daten
   und invertiert, Pseudozufall (xorshift, beim Prüfen neu erzeugt), 0xFFFF0000 und
   0xAAAA5555 (alle 16 Datenleitungen schalten zwischen den Halbwörtern); dann 1 MB in
   verwürfelter Zeilenfolge (Zeilenwechsel direkt nach dem Schreiben) und 256 zufällige Wörter,
   jedes geschrieben und sofort ungecacht zurückgelesen.
8. **Code aus dem SDRAM:** `auipc a0, 0; ret` hineinkopieren, aufrufen, die Adresse der Kopie
   erwarten; dann viermal mit `li a0, K` überschreiben, `fence.i` und drei NOPs, aufrufen, K
   erwarten (die alte Zeile steht dabei jedes Mal noch im I-Cache). Jeder Befehl wird vorher
   als Cache-Zeile zurückgelesen, so wie der I-Cache ihn holt; was das SDRAM verfälscht hat,
   läuft nie (eine hängende Datenleitung machte sonst aus `auipc a0` ein `auipc s0`).
9. **Refresh:** `MEM_REFRESH` über 60 Bilder ab einem Bildwechsel zählen (Simulation: 2),
   mindestens 128.000 je Sekunde verlangen (erwartet 74,25 MHz / 512 = 145.020).
10. **Schreib-Status:** Bank 0 Zeile 0x800 öffnen, ein `sw` nach Zeile 0xC00 (PRECHARGE,
    ACTIVATE, 2 × WRITE: 7 Takte), sofort `MEM_STATUS` lesen (Bit 4 = 0 erwartet), warten bis
    1, Wert zurücklesen. Die drei Zugriffe stehen in einer Funktion, die genau in einer
    I-Cache-Zeile liegt: Ein Zeilen-Nachladen zwischen `sw` und `lw` dauert 16 Takte, länger
    als das Schreiben, und ließ den Test in der ersten Fassung je nach Code-Lage scheitern.
11. **Messung** (kein Test): Takte für eine Cache-Zeile mit offener und mit anderer Zeile in
    der Bank (die schnellste von vier Messungen), MB/s Lesen und Schreiben über den Cache mit
    einer ausgerollten Schleife; der gemessene Code liegt an I-Cache-Zeilen ausgerichtet.

In der Simulation (`SYS_FLAGS` Bit 0) schrumpft nur die Fenstergröße, nie der Ablauf:
geprüft werden das erste und das letzte Fenster, so laufen auch die Schleifenenden; das
Protokoll nennt die Fenster, die Testbench prüft das. Die Firmware füllt den
Stack beim Start mit einem Muster und schreibt den höchsten Stand ins Protokoll.

Ergebnis in der Simulation (alle Modelle, Fenster von 2 KB):

```
SDRAM slot: ok, cycles 474618
phase map ####
SDRAM write status: ok, cycles 13600
SDRAM line read: row hit 30 cycles, row miss 34; read 63 MB/s, write 95 MB/s
memtest: errors SDRAM 0
```

Eine Cache-Zeile kostet die CPU 30 Takte bei offener Zeile, 4 mehr (tRP + tRCD) bei einem
Zeilenwechsel in der Bank. Die Musterschleifen sind eine Schleife je Muster ohne Aufrufe,
der seltene Fehlerfall liegt außerhalb (`__builtin_expect`), damit VexRiscvs statische
Sprungvorhersage immer trifft: 5 Befehle je Wort beim Prüfen. Geschätzte Dauer auf dem Pocket
für den ganzen Speicher: SDRAM-Muster knapp 20 s.

## Prüfung

- **SDRAM-Modell** (`sim/models/sdram_model.v`), von einem eigenen Agenten allein aus dem
  Datenblatt geschrieben, mit eigenem Selbsttest (eine Negativprobe je Regel): Mode-Register
  unbekannt bis MRS und EMRS, Befehlsfolgen, alle Zeiten in Takten, Refresh-Abstand,
  DQM-Latenzen, abgeschnittene Bursts, Buskonflikte über die getrennten `dq_oe`; Speicher mit
  Giftmuster. Takt-Konvention: Das Modell sieht einen Befehl eine Flanke nach dem Controller
  (steht für φ), gibt Halbwort k ab Flanke r+CL−1+k aus; die Simulation zeigt dann Leselatenz
  CL+1 = 3. Die echte Lese-Flanke beweist nur die Timing-Analyse und die Phasenmessung.
- **Fehler-Einspeisung** (`tools/sim_faults.sh`): fünf Läufe von `sim/soc` mit je einem in
  das SDRAM-Modell eingebauten Fehler (DQM vertauscht, DQ8 hängt auf 0, BA1 hängt auf 0,
  Zeilenbit 12 folgt Bit 11, jeder 16.000. Schreib-Beat fehlt; bis zum Ausbau dazu SRAM
  UB/LB vertauscht und `cram0` Die 1 antwortet nie). `SYS_SIM` muss genau die vorhergesagten
  Tests als fehlgeschlagen zeigen, nicht mehr und nicht weniger (die Begründung je Fall steht
  im Skript), und das Protokoll muss einen Fehler melden.
- **Zufallstest** (`sim/sdram`): `sdram_port` + `sdram.v` + Modell, Busmodelle genau nach dem
  Protokoll der CPU. Adressen aus einem kleinen Pool (wenige Zeilen je Bank, Zeilen- und
  Bankgrenzen, letzte Zeile), Pausen meist 0–3 Takte, Schreiben gefolgt von Lesen derselben
  Zeile, iBus und dBus im selben Takt, Schreib-Bursts, alle sieben Masken, Teilwort-Lesen,
  CPU-Resets von 1 Takt bis länger als ein Auftrag zu zufälligen Zeiten, Bridge-Wörter (mit 88
  Takten Abstand; ein gezielter schneller Burst muss das Überlaufbit setzen), iBus-Zeilen
  außerhalb des SDRAM. Prüfungen: genaue Wortzahl, erstes Wort frühestens einen Takt nach der
  Annahme, kein unangefordertes Wort, Daten gleich einem Schattenspeicher nach der
  Reihenfolge-Regel, Befehlsreihenfolge am SDRAM gleich Annahmereihenfolge, Statusbit
  „erledigt" nie 1 bei offenem Schreibzugriff, kein Modellfehler; ein Watchdog meldet einen
  Auftrag, der 20.000 Takte keine Antwort bekommt. Dazu etwa alle 20.000 Takte eine kurze
  Neuinitialisierung mitten im Verkehr. Abdeckungszähler je Fall; Seeds 1–4 mit je 2 Millionen
  Takten bestehen (je rund 100 Neuinitialisierungen, längste Refresh-Lücke 550 Takte).
- **Mutationsprüfung des Zufallstests:** 14 absichtlich eingebaute Fehler in `sdram.v` und
  `sdram_port.v`, jeder muss den Zufallstest scheitern lassen, und jeder tut es: iBus vor
  dBus im Arbiter (iBus liest alte Daten), Halte-Takt nach READ entfernt (abgeschnittene
  Bursts), Epoche entfernt (Wort ohne Anfrage nach einem Reset), Statusbit ohne das
  Port-Register, Refresh alle 600 Takte, tRCD 1, tWR 1, READ→WRITE nach 3 Takten
  (Buskonflikt), Einzel-Lesen an falscher Spalte, nur das untere Halbwort geschrieben, Maske
  ignoriert, Leselatenz 2, PRECHARGE an die falsche Bank, Refresh vor Ablauf von tRP.
- **Systemtest** (`sim/soc`): die echte Firmware mit allen Modellen; die Bridge wie der Pocket
  (88 Takte Abstand, vor jedem Slot Warten auf die Quittung), Test-Slot geladen; ein Reset
  mitten im SDRAM-Verkehr; der letzte Lauf meldet den Slot-Test als übersprungen. Alle 13
  Bits in `SYS_SIM` (`0x80001FFF`), Modell ohne Fehler, längste Refresh-Lücke 533 Takte.
- **Timing-Analyse:** IO-Register auf allen 103 Speicher-Pins gepackt, die vier
  SDRAM-Übergänge in allen Ecken mit Abstand (Tabelle oben), Phase in ps mit VCO und Zählern
  dokumentiert. Belegung mit allen drei Controllern: 4.212 ALMs (23 %), 194 von 308 M10K
  (63 %), 11 DSP, 2 PLLs.
- **Pocket:** Speichertest, Phasen-Fenster und Ergebnis aus `debug.txt`; danach den Test eine
  Weile wiederholt laufen lassen.
