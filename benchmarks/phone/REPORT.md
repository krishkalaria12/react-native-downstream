# Phone streaming benchmark

CPH2569 · Android 15 · 60 Hz · arm64 Hermes release · 2026-10-13. 252 measured runs, 24 warmups excluded. Every number is the median of 10 cold-process runs, with its 95% bootstrap confidence interval in brackets. Lower is better everywhere.

## Workloads

| Workload             | What streams                                                    | Replies at once | Runs per library |
| -------------------- | --------------------------------------------------------------- | --------------: | ---------------: |
| ai-single            | One AI reply (prose, table, TypeScript)                         |               1 |               10 |
| ai-code-heavy        | One code-heavy AI reply (14 code blocks)                        |               1 |               10 |
| ai-feature-matrix    | Math, code and tables: every subset and ordering                |               1 |               10 |
| ai-concurrent-5      | Five AI replies streaming at once, including the feature matrix |               5 |               10 |
| ai-huge-single       | One huge AI reply (60 points, mixed features)                   |               1 |                1 |
| ai-huge-concurrent-5 | Five huge AI replies (60 points each, mixed features)           |               5 |                1 |

Huge workloads run once per library, so their rows have no confidence interval or significance test.

Live: every run asks `gpt-6-luna` (reasoning medium) for fresh replies through the Vercel AI SDK `streamText` on the Mac, and each text delta reaches the phone over USB the moment it arrives. Replies differ from run to run, so "CPU per KiB" normalizes CPU by the text each run actually streamed.

## Code colors on (library defaults)

### ai-single: One AI reply (prose, table, TypeScript)

| Library           |         App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | ------------------: | ---------------: |
| **Downstream**    |    6.00 [5.72–6.40] |  1.84 [1.66–1.96] |           12.5 [10.7–16.5] |                       14.2 [11.9–19.5] |    14.7 [13.1–16.4] |    19.5 [18.2–21.8] |                   30 [0–320] |       4.6 [4.5–4.8] | 1304 [1195–1392] |
| RN Streamdown     |    8.53 [7.73–9.63] |  2.18 [2.03–2.41] |           22.8 [21.7–23.9] |                         7.8 [6.6–10.7] |    10.9 [10.6–11.1] |    12.0 [11.8–12.3] |                  85 [34–107] |       4.5 [4.3–4.7] | 1932 [1692–2179] |
| Enriched Markdown |    8.66 [7.89–9.44] |  2.57 [2.38–2.94] |           14.6 [13.8–16.3] |                         9.1 [8.2–10.5] |    10.7 [10.6–11.5] |    12.7 [11.6–14.3] |                   24 [15–42] |       4.6 [4.5–4.8] | 1815 [1666–2008] |
| Markdown Display  |  11.45 [9.13–13.65] |  4.47 [3.67–5.28] |           37.7 [35.1–39.6] |                       66.7 [62.5–74.0] |    43.3 [36.2–52.5] |    56.4 [48.6–67.9] |                  96 [42–124] |       4.6 [4.4–5.0] | 2400 [2063–2826] |
| Marked            | 11.46 [11.06–14.05] |  5.24 [4.84–6.29] |           43.8 [39.1–51.5] |                       69.4 [58.2–74.5] |   82.4 [59.7–101.6] |  106.1 [71.5–140.9] |                 111 [64–174] |       4.7 [4.5–4.9] | 2551 [2327–2921] |
| Markdown Renderer | 11.46 [10.17–13.75] |  5.32 [4.67–6.63] |           40.6 [33.9–47.7] |                       75.5 [69.5–80.7] |    66.1 [54.8–93.5] |   89.1 [73.3–132.3] |                 109 [79–135] |       4.7 [4.5–4.8] | 2543 [2218–2872] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |     Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | -----------------: |
| RN Streamdown     | 1.42×* [1.25–1.63] | 1.19×* [1.07–1.39] |   1.83×* [1.38–2.20] |                 0.55×* [0.38–0.75] | 0.62×* [0.56–0.67] |
| Enriched Markdown | 1.44×* [1.27–1.54] | 1.40×* [1.27–1.60] |    1.17× [0.88–1.42] |                 0.64×* [0.46–0.79] | 0.65×* [0.56–0.74] |
| Markdown Display  | 1.91×* [1.48–2.28] | 2.43×* [1.97–2.95] |   3.02×* [2.28–3.55] |                 4.69×* [3.32–5.74] | 2.89×* [2.39–3.46] |
| Marked            | 1.91×* [1.77–2.39] | 2.85×* [2.57–3.48] |   3.51×* [2.62–4.38] |                 4.88×* [3.40–5.73] | 5.45×* [3.65–7.23] |
| Markdown Renderer | 1.91×* [1.72–2.31] | 2.90×* [2.55–3.70] |   3.25×* [2.40–4.07] |                 5.31×* [3.74–6.35] | 4.57×* [3.65–6.84] |

### ai-code-heavy: One code-heavy AI reply (14 code blocks)

| Library           |         App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | ------------------: | ---------------: |
| **Downstream**    |    6.75 [6.24–7.13] |  2.19 [2.02–2.37] |           15.5 [13.7–17.9] |                       25.6 [22.4–41.1] |    18.3 [17.5–22.9] |    25.6 [20.5–30.6] |                   37 [0–810] |       4.5 [4.1–4.7] | 1465 [1382–1700] |
| RN Streamdown     |  10.67 [9.68–11.80] |  2.46 [2.27–2.65] |           26.1 [24.2–28.6] |                        11.7 [9.5–15.3] |    11.4 [11.2–11.7] |    16.8 [13.0–17.7] |                     0 [0–38] |       4.7 [4.4–4.9] | 2273 [2200–2429] |
| Enriched Markdown |   9.82 [7.30–11.18] |  2.26 [1.68–2.57] |           17.3 [17.1–18.2] |                        10.7 [7.9–13.3] |    11.8 [11.4–12.4] |    17.8 [16.9–18.3] |                     0 [0–35] |       4.3 [4.1–4.5] | 2351 [1685–2572] |
| Markdown Display  | 15.01 [12.60–19.02] |  6.49 [5.47–8.57] |           43.0 [41.4–44.2] |                       85.8 [80.4–89.2] |   68.2 [64.1–102.7] |   96.9 [83.1–133.0] |                  97 [77–135] |       4.2 [3.9–4.5] | 3594 [3109–4468] |
| Marked            | 16.58 [13.61–20.76] |  7.80 [6.17–9.97] |           81.9 [74.2–91.8] |                       84.5 [82.0–90.2] |  101.2 [82.8–131.7] | 154.9 [107.9–232.8] |                 130 [31–246] |       4.5 [4.3–4.7] | 3752 [3193–4315] |
| Markdown Renderer | 16.61 [13.08–18.44] |  8.18 [6.61–9.45] |           56.3 [49.7–62.5] |                       86.7 [79.8–90.1] | 118.6 [103.1–157.8] | 198.9 [140.5–230.4] |                 147 [93–167] |       4.4 [4.2–4.7] | 3579 [2852–4319] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |      Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | ------------------: |
| RN Streamdown     | 1.58×* [1.40–1.78] | 1.12×* [1.02–1.26] |   1.69×* [1.45–1.94] |                 0.46×* [0.27–0.62] |  0.66×* [0.50–0.83] |
| Enriched Markdown | 1.45×* [1.08–1.68] |  1.03× [0.76–1.23] |   1.12×* [0.97–1.27] |                 0.42×* [0.23–0.56] |  0.69×* [0.57–0.87] |
| Markdown Display  | 2.22×* [1.86–2.91] | 2.96×* [2.47–3.98] |   2.78×* [2.41–3.15] |                 3.36×* [1.99–3.87] |  3.78×* [3.05–5.57] |
| Marked            | 2.46×* [2.01–3.15] | 3.56×* [2.77–4.57] |   5.30×* [4.55–6.15] |                 3.30×* [2.02–3.85] |  6.05×* [4.13–9.25] |
| Markdown Renderer | 2.46×* [1.93–2.81] | 3.73×* [2.98–4.42] |   3.64×* [3.08–4.20] |                 3.39×* [2.02–3.90] | 7.77×* [5.34–10.09] |

### ai-feature-matrix: Math, code and tables: every subset and ordering

| Library           |         App CPU (s) |   UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) |  Frame time p99 (ms) | Settle after last chunk (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ------------------: | -------------------------: | -------------------------------------: | ------------------: | -------------------: | ---------------------------: | ------------------: | ---------------: |
| **Downstream**    |  10.20 [9.13–11.69] |    3.16 [2.91–3.51] |           17.3 [16.9–20.1] |                       19.0 [14.6–25.8] |    17.1 [15.4–18.3] |     20.2 [19.7–25.1] |                728 [30–1215] |       6.3 [5.7–6.8] | 1681 [1497–1753] |
| RN Streamdown     | 17.26 [16.09–20.18] |    3.02 [2.84–3.16] |           36.3 [35.1–37.5] |                         9.9 [9.0–17.1] |    11.0 [10.5–11.5] |     16.1 [12.6–17.3] |                169 [110–549] |       5.6 [5.1–6.1] | 3084 [2934–3404] |
| Enriched Markdown | 18.49 [16.28–20.42] |    3.44 [3.26–4.09] |           24.9 [24.4–25.2] |                         9.4 [8.8–12.6] |    11.0 [10.5–11.2] |     15.9 [14.1–17.5] |                  97 [80–109] |       6.0 [5.6–6.7] | 3063 [2811–3325] |
| Markdown Display  | 29.59 [25.07–32.67] | 13.17 [11.20–15.09] |           57.9 [54.9–63.2] |                       77.1 [73.7–88.7] | 182.7 [146.0–232.4] |  349.3 [264.7–393.8] |                343 [187–663] |       6.1 [5.8–6.6] | 4781 [4207–5245] |
| Marked            | 30.14 [27.19–33.97] | 14.86 [13.63–16.66] |        111.7 [106.1–126.3] |                       72.0 [63.7–77.2] | 254.1 [208.9–324.2] |  524.8 [373.8–808.5] |              1063 [185–1554] |       6.0 [5.4–6.5] | 5170 [4544–5534] |
| Markdown Renderer | 31.33 [26.68–37.04] | 16.49 [14.04–19.40] |        162.3 [138.3–170.4] |                       75.5 [67.1–83.9] | 249.3 [149.5–322.4] | 694.8 [428.2–1348.3] |              1162 [208–2231] |       5.5 [5.1–6.0] | 6036 [4732–6317] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |        Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | --------------------: |
| RN Streamdown     | 1.69×* [1.45–1.98] |  0.95× [0.84–1.03] |   2.10×* [1.79–2.18] |                 0.52×* [0.38–0.91] |    0.80×* [0.54–0.84] |
| Enriched Markdown | 1.81×* [1.51–2.09] | 1.09×* [0.97–1.35] |   1.44×* [1.23–1.47] |                 0.50×* [0.37–0.70] |    0.79×* [0.62–0.87] |
| Markdown Display  | 2.90×* [2.35–3.33] | 4.17×* [3.43–4.89] |   3.34×* [2.84–3.54] |                 4.05×* [2.94–5.22] | 17.33×* [10.88–19.14] |
| Marked            | 2.96×* [2.50–3.44] | 4.70×* [4.03–5.45] |   6.44×* [5.56–7.31] |                 3.78×* [2.71–4.73] | 26.04×* [16.03–40.12] |
| Markdown Renderer | 3.07×* [2.49–3.65] | 5.22×* [4.30–6.18] |   9.37×* [7.41–9.94] |                 3.96×* [2.87–5.02] | 34.47×* [19.11–66.90] |

### ai-concurrent-5: Five AI replies streaming at once, including the feature matrix

| Library           |         App CPU (s) |   UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) |   Frame time p99 (ms) | Settle after last chunk (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ------------------: | -------------------------: | -------------------------------------: | ------------------: | --------------------: | ---------------------------: | ------------------: | ---------------: |
| **Downstream**    | 26.68 [24.14–29.18] |    6.45 [5.72–7.24] |           46.0 [43.4–50.1] |                       16.5 [14.1–17.9] |    17.7 [16.9–19.5] |      22.3 [21.5–26.5] |                 72 [30–1231] |    25.1 [24.3–25.7] |  1063 [950–1118] |
| RN Streamdown     | 42.68 [35.77–44.74] |    7.90 [7.05–8.96] |           62.1 [57.3–63.4] |                        11.0 [5.1–12.1] |    13.0 [12.0–14.6] |      18.8 [15.9–21.2] |                 118 [51–364] |    25.0 [24.3–25.7] | 1661 [1470–1752] |
| Enriched Markdown | 39.87 [31.67–42.08] |    8.37 [7.68–9.87] |           37.7 [35.7–38.3] |                        13.3 [7.2–16.8] |    13.7 [11.1–17.2] |      19.5 [15.0–22.8] |                  71 [35–101] |    24.7 [23.3–25.7] | 1562 [1297–1718] |
| Markdown Display  | 50.39 [46.79–64.82] | 21.38 [19.59–28.21] |           89.2 [82.9–91.9] |                       82.8 [78.0–83.8] | 165.2 [150.7–188.5] |   407.9 [318.0–476.0] |                 313 [88–587] |    24.5 [24.0–25.3] | 2111 [1898–2644] |
| Marked            | 55.30 [49.81–58.95] | 24.60 [22.01–26.36] |         100.9 [98.1–103.3] |                       81.1 [78.5–86.4] | 154.9 [117.5–262.3] |   288.8 [192.2–456.4] |                  87 [71–110] |    25.5 [24.4–25.8] | 2220 [1966–2336] |
| Markdown Renderer | 57.81 [48.84–62.10] | 27.97 [23.77–29.86] |        170.7 [159.2–193.7] |                       75.0 [69.0–81.8] | 260.0 [172.3–355.9] | 1060.2 [630.4–1498.9] |                684 [61–1826] |    24.9 [24.2–26.0] | 2280 [2050–2469] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |        Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | --------------------: |
| RN Streamdown     | 1.60×* [1.34–1.83] | 1.22×* [1.06–1.46] |   1.35×* [1.20–1.43] |                 0.66×* [0.31–0.80] |    0.84×* [0.69–0.95] |
| Enriched Markdown | 1.49×* [1.15–1.70] | 1.30×* [1.11–1.61] |   0.82×* [0.74–0.86] |                  0.81× [0.42–1.07] |    0.87×* [0.64–1.03] |
| Markdown Display  | 1.89×* [1.68–2.58] | 3.31×* [2.82–4.55] |   1.94×* [1.74–2.06] |                 5.00×* [4.56–5.89] | 18.29×* [13.46–21.38] |
| Marked            | 2.07×* [1.84–2.39] | 3.81×* [3.31–4.37] |   2.20×* [1.98–2.34] |                 4.91×* [4.52–5.78] |  12.95×* [8.49–20.50] |
| Markdown Renderer | 2.17×* [1.79–2.49] | 4.34×* [3.48–4.90] |   3.71×* [3.30–4.21] |                 4.53×* [4.08–5.48] | 47.55×* [27.25–67.33] |

### ai-huge-single: One huge AI reply (60 points, mixed features)

| Library           | App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | ------------------: | ---------------: |
| **Downstream**    |       26.05 |              7.22 |                       38.7 |                                   24.5 |                18.1 |                22.9 |                           43 |                15.4 |             1687 |
| RN Streamdown     |       49.82 |              6.93 |                       71.1 |                                   11.5 |                11.5 |                13.6 |                            0 |                16.6 |             2996 |
| Enriched Markdown |       51.38 |              9.54 |                       26.2 |                                   12.2 |                12.1 |                15.9 |                            0 |                15.8 |             3250 |
| Markdown Display  |       84.86 |             39.60 |                      154.7 |                                   79.6 |               267.5 |              1763.6 |                         2318 |                15.9 |             5353 |
| Marked            |       96.07 |             45.08 |                      178.0 |                                   81.7 |               425.4 |              1987.9 |                            0 |                17.3 |             5549 |
| Markdown Renderer |       99.33 |             51.60 |                      304.5 |                                   96.7 |               410.7 |              3697.1 |                            0 |                16.3 |             6109 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | UI thread CPU | Peak memory increase | Janky frames (app deadline missed) | Frame time p99 |
| ----------------- | ------: | ------------: | -------------------: | ---------------------------------: | -------------: |
| RN Streamdown     |   1.91× |         0.96× |                1.83× |                              0.47× |          0.59× |
| Enriched Markdown |   1.97× |         1.32× |                0.68× |                              0.50× |          0.69× |
| Markdown Display  |   3.26× |         5.49× |                4.00× |                              3.25× |         76.90× |
| Marked            |   3.69× |         6.25× |                4.60× |                              3.33× |         86.69× |
| Markdown Renderer |   3.81× |         7.15× |                7.86× |                              3.94× |        161.22× |

### ai-huge-concurrent-5: Five huge AI replies (60 points each, mixed features)

| Library           | App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | ------------------: | ---------------: |
| **Downstream**    |       75.57 |             20.26 |                      135.2 |                                   41.4 |                38.5 |                51.0 |                         2937 |                73.7 |             1025 |
| RN Streamdown     |      104.50 |             15.52 |                      121.7 |                                   28.6 |                19.9 |                27.7 |                            0 |                78.6 |             1329 |
| Enriched Markdown |       80.47 |             15.10 |                       73.8 |                                   35.7 |                22.1 |                32.5 |                          130 |                80.0 |             1006 |
| Markdown Display  |      112.12 |             48.88 |                      246.2 |                                   74.7 |               341.7 |              2451.2 |                            0 |                80.1 |             1399 |
| Marked            |       99.09 |             41.78 |                      106.1 |                                   91.4 |               524.7 |               687.5 |                           89 |                73.1 |             1355 |
| Markdown Renderer |      121.87 |             58.84 |                      405.6 |                                   72.0 |               282.6 |              2101.9 |                            0 |                76.2 |             1599 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | UI thread CPU | Peak memory increase | Janky frames (app deadline missed) | Frame time p99 |
| ----------------- | ------: | ------------: | -------------------: | ---------------------------------: | -------------: |
| RN Streamdown     |   1.38× |         0.77× |                0.90× |                              0.69× |          0.54× |
| Enriched Markdown |   1.06× |         0.75× |                0.55× |                              0.86× |          0.64× |
| Markdown Display  |   1.48× |         2.41× |                1.82× |                              1.80× |         48.08× |
| Marked            |   1.31× |         2.06× |                0.78× |                              2.21× |         13.49× |
| Markdown Renderer |   1.61× |         2.90× |                3.00× |                              1.74× |         41.23× |

## Features

| Feature                                                   |    Downstream    |      RN Streamdown       |         Enriched         |  Markdown Display  |       Marked       | Markdown Renderer  |
| --------------------------------------------------------- | :--------------: | :----------------------: | :----------------------: | :----------------: | :----------------: | :----------------: |
| Native text rendering                                     |       yes        |           yes            |           yes            | no (RN Text tree)  | no (RN Text tree)  | no (RN Text tree)  |
| Append-only stream API (append / finish)                  |       yes        |            no            |            no            |         no         |         no         |         no         |
| Reuses finished blocks while streaming                    |       yes        |    no, full re-parse     |    no, full re-parse     |         no         |         no         |         no         |
| Repairs unfinished Markdown mid-stream                    | yes, per syntax  |       yes (remend)       |            no            |         no         |         no         |         no         |
| Streaming reveal animation                                |       yes        |           yes            |           yes            |         no         |         no         |         no         |
| Streaming caret                                           |       yes        |            no            |            no            |         no         |         no         |         no         |
| Syntax highlighting                                       |  yes, built in   | yes, build-time grammars | yes, build-time grammars |         no         |  custom renderer   |         no         |
| Code block line numbers, filename, download, wrap, expand |       yes        |        copy only         |        copy only         |         no         |         no         |         no         |
| Table copy, export and full-screen                        |       yes        |            no            |            no            |         no         |         no         |         no         |
| Footnotes                                                 |       yes        |            no            |            no            |         no         |         no         |         no         |
| Alerts / admonitions                                      |       yes        |           yes            |           yes            |         no         |         no         |         no         |
| Math                                                      | optional package |           yes            |           yes            |         no         |  custom tokenizer  |         no         |
| Collapsible <details>                                     |       yes        |            no            |            no            |         no         |         no         |         no         |
| Your own React components in fences and tags              |       yes        |            no            |            no            | per-node overrides | per-node overrides | per-node overrides |

- Enriched Markdown has streaming display options (`streamingAnimation`, table and code-block modes) but no append API: every update passes the whole string and md4c re-parses it, and unfinished syntax is not repaired. Its docs point to RN Streamdown for that.
- RN Streamdown runs remend on a worklet thread, then hands the whole repaired string to Enriched on every update.
- Checked against the installed packages: react-native-streamdown 0.3.0, react-native-enriched-markdown 1.1.1, react-native-markdown-display 7.0.2, react-native-marked 8.3.2, react-native-markdown-renderer 4.1.1.

## How it was measured

- **Live input.** One release APK holds every library. Each run is a cold process showing one library full screen (five stacked panes for the concurrent workload, all visible). The Mac streams fresh model replies and forwards each delta over USB as it arrives; the app appends it immediately and the harness checks that the phone rendered exactly the text the model streamed. The window starts at the first delta, so model thinking time is not measured.
- **Feature coverage.** New captures and live runs request all eight subsets of math, code and tables, including every ordering, in one 16-section reply. Existing replies also mix nested formatting, lists, tasks, links, alerts, footnotes and Unicode. Huge replies rotate the six three-feature orderings. This covers content combinations, not every renderer option. Model output can omit requested constructs; inspect the recorded or live text before claiming complete coverage. Older captures retain their original content and must be recaptured for this matrix.
- **Math.** Math parsing is enabled for the matrix, concurrent and huge workloads. Downstream uses its installed math package; Streamdown and Enriched use their native math engines. Other libraries receive the same source with their default handling. Unsupported features can remain literal, so rendering and performance are not equivalent.
- **Frames.** Perfetto records SurfaceFlinger FrameTimeline, which classifies every frame the app draws. "Janky (app deadline missed)" is the share of frames Android itself blames on the app; "any cause" adds SurfaceFlinger, display and prediction misses but not buffer or SurfaceFlinger stuffing (queueing latency, not a visible stutter). Frame time is FrameTimeline's actual frame duration (CPU start to GPU done or post). Deadline overrun is actual end minus expected end, as in Jetpack Macrobenchmark.
- **CPU.** Scheduler slices for every thread of the app process inside the measured window, split into the UI thread, the JS thread (mqt_js), RenderThread and everything else (native workers, GC, worklets). This is exact on-CPU time, not sampling.
- **Memory.** Anonymous RSS plus swap, polled by Perfetto every 100 ms. "Peak increase" is the highest value during the window minus the value when streaming started. PSS from `dumpsys meminfo` is read after the window so it cannot disturb it.
- **Settle.** Time from the last delivered chunk to the last frame the app drew, from the trace.
- **JS lateness.** How late the app delivered each chunk against its schedule, a proxy for a busy JS thread.
- **Window.** From the moment streaming starts until 2 s after the last chunk. App start-up and mounting are outside it.
- **Order and noise.** 10 measured blocks plus one warmup block; every block runs each library and workload once in a seeded random order (seed 1). Before each run the app is force-stopped, background apps are killed and the harness waits until the thermal status is 0 and the battery is at most 37.4 °C. App code is AOT-compiled (`cmd package compile -m speed`). The screen stays on at fixed brightness with the refresh rate pinned.
- **Statistics.** Medians of per-run values, percentile bootstrap 95% intervals (4,000 resamples). Ratios are bootstrapped from both groups. Mann-Whitney U is two-sided with a normal approximation; no multiple-comparison correction, so read single stars with care.
- **Limits.** One phone model. The common app contains every library, so memory is not a per-package size. Clock speeds are not locked (needs root). Markdown output differs between libraries (see Features); equal input is not equal rendering.

Raw runs, traces and the full statistics are in `results/bench-20261013T102344` (summary.json). Build: `highlighted 9dd3a68000dd`, revision `5264202a67` with local changes.
