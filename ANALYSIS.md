# Analisi dei due codebase — gifsicle e flexigif

Documento di studio della fase 1. Ogni verdetto dice se è **misurato** (numero
riproducibile con gli strumenti in `bench/`) o **letto** (dedotto dal sorgente).
Corpus e metodo: vedi `CLAUDE.md`.

## 0. Prima di tutto: "lossless" è ambiguo, e i due tool stanno su due livelli diversi

Questa è la scoperta che ordina tutto il resto.

| livello | cosa è invariante | chi ci lavora | verificato con |
|---|---|---|---|
| **L1 — struttura** | stesso numero di frame, stessa geometria per frame, stessi indici, stessa palette | flexigif (riscrive *solo* il bitstream LZW) | `stage_bench cmp` |
| **L2 — rendering** | stessa animazione riprodotta: stessi pixel su canvas nel tempo, stessa durata totale | gifsicle `-O1/-O2/-O3`, de-interlacciamento | `gifdiff` |

L2 contiene L1. Sono livelli, non alternative: L2 riscrive la struttura (ritaglia i
frame, cambia disposal, rende trasparenti i pixel invariati, fonde i frame
ridondanti, pota la palette) e L1 riscrive la codifica di ciò che resta.

Due conseguenze misurate:

* Su `l02` e `l07` gifsicle `-O3` **elimina frame ridondanti** (475→425 e 288→242)
  sommando i delay al frame precedente: durata totale identica (9500 ms e 11520 ms
  invariati), `gifdiff -w` dice identico, `gifdiff` senza `-w` dice diverso.
* Il `raw_equal` di chisel confronta numero di frame e delay per-frame via stb:
  classificherebbe l'output `gifsicle -O3` come **corruzione**. Quindi il livello di
  losslessness non è un dettaglio interno: è un parametro pubblico del progetto.

## 1. Misure trasversali (le tabelle che decidono i verdetti)

Corpus di 51 GIF reali (31,6 MB, 136,5 M pixel, 1–475 frame). M2 Pro, Release.

**Intero corpus, L2 permesso:**

| | totale | vs originale | tempo |
|---|---|---|---|
| originale | 31 625 992 B | — | — |
| gifsicle come lo chiama chisel (`flags = 0`) | 30 798 799 B | −2,62 % | 1,5 s |
| gifsicle `-O3` | 29 082 967 B | **−8,04 %** | **4,8 s** |

Sui soli 14 file animati: −0,40 % (chisel) contro **−8,27 %** (`gifsicle -O3`).

**Sottoinsieme di 29 file ≤ 600 k pixel** (l'unico su cui flexigif finisce in
tempi umani):

| | totale | vs originale |
|---|---|---|
| originale | 1 359 632 B | — |
| pipeline chisel di oggi (gifsicle + flexigif) | 1 204 238 B | −11,43 % |
| gifsicle `-O3` da solo | 1 230 957 B | −9,46 % |
| **gifsicle `-O3` + flexigif** | **1 194 813 B** | **−12,12 %** |

**Le due ottimizzazioni compongono**: il DP LZW aggiunge 0,69 pp sopra `gifsicle -O3`, e
`gifsicle -O3` aggiunge 2,66 pp sopra quello che chisel ottiene oggi. In tempo: `gifsicle -O3` costa
meno di 1 s su questi 29 file, flexigif ~737 s.

**Perché `gifsicle -O3` guadagna anche su immagini a frame singolo** (non è ottimizzazione
d'animazione — è potatura della palette, misurata):

| file | palette | byte | LZW |
|---|---|---|---|
| `xs07_Relfizeau` originale | 256 colori | 5 060 | 4 260 |
| dopo `gifsicle -O3` | **8 colori** | 4 151 | 4 103 |
| `xs01_Dictionary…` originale | 256 | 5 866 | 5 066 |
| dopo `gifsicle -O3` | **8** | 4 972 | 4 916 |

768 B di tabella colori che scendono a 24, più un `min_code_size` più piccolo che
migliora anche l'LZW. flexigif non può farlo per costruzione: ricopia gli header
verbatim.

**De-interlacciamento** (7 file su 51 sono interlacciati), `gifdiff -w` identico:

| file | `gifsicle -O3` | `-O3 --no-interlace` |
|---|---|---|
| `m02_Rrfs…` | 65 436 | 59 634 (−8,87 %) |
| `m08_Rrfs…` | 47 364 | 43 134 (−8,93 %) |
| `s08_Census…` | 48 033 | 45 259 (−5,78 %) |

**Robustezza del parser**: gifsicle legge **51/51**. flexigif ne rifiuta **4**
(`too few bits available in unlzw` ×2, `there is still some data left ...`,
`expected local descriptor, but not found`); l'output di gifsicle ne recupera 3.

**Parsing non-greedy di flexigif** (disattivato da chisel), `alignment = 40`:

| file | greedy | non-greedy | costo |
|---|---|---|---|
| `xs09_Kim_Sue-ho` | 11 624 | 11 594 (−0,26 %) | 0,7× |
| `xs08_Ribbon…` | 10 299 | **11 161 (+8,37 %)** | 2,5× |
| `s02_CoA_of_Țarigrad` | 23 639 | 23 487 (−0,64 %) | 3,2× |

Il +8,37 % non è rumore: `merge()` ricalcola `optimize.greedy` per blocco
(`m_best[...].nongreedy == 0`), quindi il pass di emissione può usare un parsing
diverso da quello con cui il DP ha calcolato il costo. Modello di costo e
emissione possono divergere.

**Dove va il tempo di flexigif** (`xs02_All_FDIC`, 230 053 px, 28,2 s):
pre-pass 28 239 ms, final pass 3 ms, decode+write ~1 ms. Dentro il pre-pass:
23 011 chiamate, 134,9 M token, **5,0 G load dipendenti** su una tabella da 4,2 MB
(2,57 G in `findMatch` + 2,44 G in `addCode`, che ripercorre lo stesso cammino),
e solo il 5,1 % nel memset del dizionario.

## 1bis. Il DP sulle animazioni, dopo `gifsicle -O3`

Misurato su tre animazioni di taglia media già ottimizzate `gifsicle -O3`, `alignment = 160`
(le animazioni grandi non sono misurabili con l'implementazione attuale: `l02` sono
31,5 Mpx, ~87 h anche ad `alignment = 160`):

| file | frame | `gifsicle -O3` vs orig | DP sopra `gifsicle -O3` | pixel: orig → dopo `gifsicle -O3` | tempo DP |
|---|---|---|---|---|---|
| `l06_CD86_structure` | 50 | −3,80 % | **−1,01 %** | 4 500 000 → 2 147 100 (−52 %) | 3,3 s |
| `l01_Wikinews…logo` | 44 | −32,04 % | **−1,09 %** | 7 040 000 → 3 548 900 (−50 %) | 6,1 s |
| `l04_Sidr…` | 12 | −1,06 % | **−3,48 %** | 5 760 000 → 5 645 600 (−2 %) | 21,2 s |

Due conclusioni, entrambe a favore dell'ordine "prima strutturale, poi LZW":

1. Il DP resta utile sulle animazioni (−1 % / −3,5 % sopra `gifsicle -O3`): non degrada a
   funzione per sole immagini statiche.
2. `gifsicle -O3` **dimezza il lavoro del DP** ritagliando i frame al bounding box: su due
   dei tre file i pixel da ottimizzare scendono del ~50 %. Lo stadio caro diventa
   più economico proprio perché quello economico gira prima.

## 2. gifsicle — mappa modulo per modulo

21 172 righe C totali; il nucleo che ci riguarda sono ~4 400.

| modulo | LOC | cosa fa | verdetto |
|---|---|---|---|
| `gifread.c` | 979 | parser tollerante: error handler con recovery (clampa `min_code_size` fuori range, scarta frame a dimensione zero o oltre 65535², tronca i messaggi dopo N errori, conserva estensioni sconosciute), letture `COMPRESSED`/`UNCOMPRESSED` | **PORTA** — misurato: unico dei due che legge 51/51 |
| `giffunc.c` | 844 | modello dati: `Gif_Stream`/`Gif_Image`/`Gif_Colormap`/`Gif_Comment`/`Gif_Extension`, refcount, coesistenza di rappresentazione compressa e non | **PORTA (concetto)** — è esattamente il modello che manca a flexigif; da riscrivere in C++ con RAII invece di refcount manuale |
| `optimize.c` + `opttemplate.c` | 467+960 | ottimizzatore d'animazione: simulazione del canvas, bounding box minimo per frame, scelta del disposal con modello a penalità, diffing con trasparenza (a `gifsicle -O3` prova due varianti e tiene la più piccola), costruzione colormap globale/locali, rimozione dei frame ridondanti con somma dei delay | **PORTA** — è il modulo col miglior rapporto guadagno/tempo di tutto lo studio, e chisel non lo invoca mai |
| `gifwrite.c` | 1213 | writer del container + encoder LZW greedy con restart euristico (media mobile EWMA delle run, con *rewind* al punto di clear scelto), flag `CAREFUL_MIN_CODE_SIZE`/`EAGER_CLEAR`/`OPTIMIZE`/`SHRINK`, blocking a 255 B, calcolo di `min_code_bits` | **PORTA il writer, RISCRIVI l'encoder** — l'euristica EWMA resta come encoder veloce di default e come funzione di costo per le scelte dell'ottimizzatore strutturale |
| `gifunopt.c` | 238 | *unoptimize*: riespande ogni frame a schermo intero applicando il disposal | **PORTA** — prerequisito per ri-ottimizzare animazioni già ottimizzate e per confrontare a livello L2 |
| `kcolor.c` | 708 | spazio colore e istogramma (`kchist`), usato da `optimize.c` per costruire la colormap | **PORTA parziale** — serve `kchist`; le conversioni Oklab/gamma servono solo alla quantizzazione |
| `quantize.c` | 1477 | riduzione colori lossy (median cut, diversity), dithering | **SCARTA** — lossy |
| `xform.c` | 1405 | crop, rotate, resize, sostituzione colori | **SCARTA** — fuori scopo |
| `merge.c` | 403 | fusione di più GIF in un unico stream | **SCARTA** — funzione multi-file della CLI |
| `support.c` | 1791 | opzioni, intervalli di frame, modalità della CLI | **SCARTA** |
| `gifsicle.c` + `clp.c` | 2225+2512 | main e parser degli argomenti | **SCARTA** |
| `ungifwrt.c` | 847 | writer senza LZW (retaggio dei brevetti) | **SCARTA** |
| `gifview.c` + `gifx.c` | 1427+852 | visualizzatore X11 | **SCARTA** |
| `gifdiff.c` | 640 | confronto visivo di due GIF | **TIENI come strumento di test** — già in uso in questo studio |
| `fmalloc.c`, `strerror.c` | 46+25 | compatibilità | **SCARTA** |

**Cosa fa bene**: tollera l'input reale; ha il modello dati giusto; l'ottimizzatore
strutturale è maturo (25 anni di casi limite: disposal, background, trasparenza,
frame vuoti) e velocissimo; l'euristica di clear EWMA è un ottimo compromesso.

**Cosa fa male**: l'encoder LZW è greedy con restart euristici, quindi lascia sul
tavolo quello che il DP recupera; C con macro-template (`opttemplate.c` incluso due
volte per `uint16_t`/`uint32_t`) e stato globale `G_THREAD_LOCAL`, difficile da
usare come libreria; **tre stati globali non thread-local** (`last_name` in
`gifread.c:670`, `default_error_handler` in `gifread.c:59`, `all_hooks` in
`giffunc.c:479`) che sono vere data race se due thread leggono in parallelo — banali
da eliminare in un port.

## 3. flexigif — mappa modulo per modulo

3 165 righe C++.

| modulo | LOC | cosa fa | verdetto |
|---|---|---|---|
| `LzwEncoder.cpp/hpp` | 625+126 | **il cuore**: DP sui punti di restart del dizionario. Per ogni posizione allineata calcola all'indietro il costo ottimo del suffisso (`m_best`), poi ricostruisce il cammino minimo e riemette. Più il parsing non-greedy | **RISCRIVI** — l'idea si tiene, l'implementazione no (vedi sotto) |
| `GifImage.cpp/hpp` | 532+150 | parser e writer: legge header grezzi e li ricopia verbatim, tiene solo gli indici decodificati per frame | **SCARTA** — 22 punti di `throw` senza recupero, 4/51 file rifiutati, e il modello "header opachi" impedisce per costruzione ogni ottimizzazione strutturale. Da salvare solo l'idea di `posInterlaced`/`setInterlacing` |
| `LzwDecoder.cpp/hpp` | 376+69 | decoder LZW, conta anche i bit compressi originali | **SCARTA** — gifsicle decodifica già; utile solo la statistica dei bit originali per diagnostica |
| `BinaryInputBuffer.cpp/hpp` | 133+58 | lettore a bit sull'intero file in memoria | **SCARTA** |
| `Compress.cpp/hpp` | 128+55 | formato `.Z` di unix compress | **SCARTA** — e con esso il flag `m_isGif` che sporca l'encoder di rami morti |
| `cli/flexiGIF.cpp` | 913 | CLI | **SCARTA** — ma la sua lista di opzioni è la mappa dei parametri da riesporre |

**Cosa fa bene**: il DP è l'idea giusta e paga (0,69 pp sopra `gifsicle -O3`, dove gifsicle
non arriva); il codice è leggibile e la struttura del costo è esplicita.

**Cosa fa male, in ordine di gravità misurata**:

1. **Fragilità del parser** — 4/51 file reali rifiutati, senza alcun recupero.
2. **`addCode` ripercorre il cammino che `findMatch` ha appena percorso** — il 49 %
   di 5,0 G load è pura duplicazione.
3. **Dizionario da 4,2 MB** (4095 × 256 × `int`) ad accesso casuale, azzerato ad
   ogni chiamata: la latenza per load è il termine dominante del costo.
4. **Non-greedy inaffidabile** — fino a +8,37 % di dimensione a 2,5× il tempo,
   perché il modello di costo e il pass di emissione possono divergere.
5. **`maxDictionary` è codice morto**: `m_maxDictionary = (1 << 12) - 1 = 4095` e
   `addCode` smette di incrementare lì, quindi `m_dictSize >= 4096` non è mai vero.
6. **`alignment = 10` è una scelta pessima**: 160 costa +0,149 % sul sottoinsieme e
   fa risparmiare 15–33× di tempo (27/27 output verificati L1-lossless).
7. Rami `.Z` (`m_isGif`) dentro il percorso caldo per un formato che non ci serve.
8. Nessun parallelismo: un GIF da 475 frame è un thread solo, e i frame sono
   indipendenti.

## 4. Architettura del progetto unificato

Il punto della fusione non è mettere insieme due sorgenti: è che **oggi il file
viene decodificato e ricodificato due volte con due modelli incompatibili**, e il
modello di flexigif (header opachi + indici) rende impossibile l'ottimizzazione che
vale di più. Un modello solo, letto una volta:

```
read (tollerante, stile gifread.c)
  → modello in memoria (stile Gif_Stream, C++ con RAII)
  → [opz.] unoptimize            ─┐  livello L2
  → [opz.] ottimizzazione strutturale:                       ─┤  bbox, disposal, trasparenza,
      colormap, frame ridondanti, de-interlacciamento         ─┘
  → encode LZW: greedy EWMA (default) → DP sui restart (opz.) ─── livello L1
  → write
```

Vincoli di progetto che emergono dallo studio:

* **Il livello di losslessness è un parametro esplicito** (L1 / L2), non una scelta
  implicita. Un consumatore come chisel, che verifica con un `raw_equal`
  struttura-sensibile, deve poter chiedere L1.
* **Serve una funzione di costo LZW veloce, separata da quella ottima.**
  L'ottimizzatore strutturale a `gifsicle -O3` sceglie fra due varianti di trasparenza
  *comprimendole entrambe*: non si può farlo col DP. Encoder greedy per le scelte
  interne, DP solo sul vincitore.
* **Parallelismo opzionale, single-thread di prima classe.** Stesso output bit per
  bit con e senza; il percorso a un thread non paga nulla per l'esistenza
  dell'altro. Frame indipendenti; e dentro un frame gli scan forward `bits(p, ·)`
  non dipendono da `m_best`, quindi sono calcolabili in parallelo e combinabili con
  un DP sequenziale all'indietro — risultato esatto, non approssimato.
* **Niente stato globale mutabile** (le tre variabili di gifsicle sono la lezione).

## 5. Cosa resta aperto

* Il pruning dello scan in avanti c'è ma nella forma debole e dimostrabile (i bit
  accumulati non calano, le code costano ≥ 0): vale circa il 7 %. Un bound più
  stretto richiede un limite inferiore sui bit per byte della coda, e resta un
  problema aperto.
* Il modello a penalità di gifsicle per la colormap globale non è portato. Dopo
  aver chiuso i due bug che sembravano dipenderne (uno slot spare condiviso invece
  di uno per frame, e l'indice trasparente scelto per frame), il divario su `-O` è
  passato da +0,190 % a +0,041 %, e con `--strip` a −0,020 %: siamo davanti a
  gifsicle. Quello che resta è quasi tutto su `l02`, dove a geometria identica il
  nostro LZW è più grande di 10 KB su 1,6 MB.
* `--deinterlace` è L2 ma cambia il caricamento progressivo su rete lenta: sta
  sotto un flag proprio, non dentro `-O`.
* Non è ancora misurato quanto valga il `--careful` di gifsicle (compatibilità con
  decoder difettosi) in termini di byte.
* Il DP sulle animazioni grandi resta O(n²/alignment): il parallelismo lo ha reso
  praticabile (corpus intero in ~1 minuto su dieci core) ma non lo ha reso più
  economico in lavoro totale.
