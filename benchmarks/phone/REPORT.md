# Phone streaming benchmark

CPH2569 · Android 15 · 120 Hz · arm64 Hermes release · 2026-10-15. 198 measured runs, 36 warmups excluded. Every number is the median of 5 cold-process runs, with its 95% bootstrap confidence interval in brackets. Lower is better everywhere.

## Workloads

| Workload             | What streams                                                    | Replies at once | Runs per library |
| -------------------- | --------------------------------------------------------------- | --------------: | ---------------: |
| ai-single            | One AI reply (prose, table, TypeScript)                         |               1 |                5 |
| ai-code-heavy        | One code-heavy AI reply (14 code blocks)                        |               1 |                5 |
| ai-feature-matrix    | Math, code and tables: every subset and ordering                |               1 |                5 |
| ai-concurrent-5      | Five AI replies streaming at once, including the feature matrix |               5 |                5 |
| ai-huge-single       | One huge AI reply (60 points, mixed features)                   |               1 |                1 |
| ai-huge-concurrent-5 | Five huge AI replies (60 points each, mixed features)           |               5 |                1 |

Huge workloads run once per library, so their rows have no confidence interval or significance test.

Live: every run asks `gpt-6-luna` (reasoning medium) for fresh replies through the Vercel AI SDK `streamText` on the Mac, and each text delta reaches the phone over USB the moment it arrives. Replies differ from run to run, so "CPU per KiB" normalizes CPU by the text each run actually streamed.

## Code colors on (library defaults), streaming animations off

### ai-single: One AI reply (prose, table, TypeScript)

30 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |         App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |    6.56 [4.58–8.73] |  1.65 [1.54–2.17] |            10.6 [9.9–11.4] |                         7.3 [4.2–17.4] |    11.6 [10.7–13.6] |    16.6 [11.9–18.4] |                     3 [0–42] |                          0 [0–0] |       4.7 [4.3–5.0] | 1348 [1066–1753] |
| RN Streamdown     |  11.83 [7.31–15.72] |  2.74 [1.94–3.29] |           29.0 [23.9–31.3] |                          2.1 [0.9–4.7] |     10.3 [9.1–12.0] |    11.3 [10.2–13.6] |                   70 [0–267] |                        0 [0–175] |       4.7 [4.4–5.2] | 2274 [1678–2996] |
| Enriched Markdown |   9.60 [6.51–15.69] |  2.84 [2.01–4.50] |           16.6 [14.5–17.5] |                          3.4 [2.8–5.7] |     10.2 [9.6–10.8] |    11.4 [11.0–12.3] |                    15 [0–43] |                          0 [0–0] |       4.5 [4.3–5.2] | 2212 [1433–3043] |
| Markdown Display  | 11.10 [10.49–14.93] |  4.14 [3.92–5.31] |           37.4 [36.0–40.2] |                       53.1 [42.7–54.7] |    33.0 [29.5–35.2] |    40.9 [33.5–44.9] |                    19 [0–56] |                          0 [0–0] |       4.6 [4.0–5.0] | 2683 [2410–3229] |
| Marked            | 13.81 [11.75–18.87] |  6.29 [5.10–8.44] |           53.5 [46.1–61.0] |                       47.6 [37.5–70.9] |    57.0 [49.2–61.0] |   79.3 [70.3–115.2] |                 109 [98–132] |                         0 [0–33] |       4.6 [4.2–5.0] | 3008 [2787–4040] |
| Markdown Renderer | 13.32 [10.85–17.61] |  6.24 [5.02–7.93] |           45.6 [36.7–57.6] |                       58.9 [52.4–62.3] |    63.4 [56.5–64.7] |   79.1 [63.5–152.9] |                  71 [30–110] |                          0 [0–0] |       4.6 [4.2–4.9] | 3190 [2515–3781] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |     Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | -----------------: |
| RN Streamdown     |  1.80× [0.93–2.88] |  1.66× [0.99–1.99] |   2.75×* [2.25–3.00] |                 0.28×* [0.11–0.75] | 0.68×* [0.59–0.96] |
| Enriched Markdown |  1.46× [0.99–2.39] | 1.72×* [1.21–2.72] |   1.57×* [1.37–1.68] |                 0.46×* [0.20–0.93] | 0.69×* [0.62–0.96] |
| Markdown Display  | 1.69×* [1.27–2.80] | 2.50×* [1.91–3.23] |   3.55×* [3.29–3.91] |                7.22×* [3.05–12.52] | 2.47×* [1.93–3.45] |
| Marked            | 2.11×* [1.53–3.54] | 3.81×* [2.62–5.28] |   5.07×* [4.37–5.78] |                6.48×* [2.74–11.33] | 4.78×* [3.96–6.95] |
| Markdown Renderer | 2.03×* [1.52–3.30] | 3.77×* [2.83–4.96] |   4.32×* [3.48–5.60] |                8.03×* [3.39–13.91] | 4.78×* [3.76–9.23] |

### ai-code-heavy: One code-heavy AI reply (14 code blocks)

30 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |         App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |    6.34 [5.43–8.11] |  1.70 [1.58–2.16] |           11.7 [11.1–12.7] |                         7.9 [3.9–11.9] |    13.9 [11.4–15.9] |    19.3 [12.5–19.6] |                      0 [0–0] |                          0 [0–0] |       4.7 [3.8–5.6] |  1536 [968–1743] |
| RN Streamdown     |  13.21 [8.35–15.40] |  2.50 [1.78–2.82] |           30.4 [24.7–32.4] |                          3.7 [1.9–4.2] |    11.0 [10.0–11.1] |    12.2 [11.4–12.7] |                     0 [0–74] |                          0 [0–0] |       4.6 [4.2–4.8] | 2885 [1971–3372] |
| Enriched Markdown |  10.72 [6.84–14.47] |  2.28 [0.06–2.71] |            17.6 [9.8–20.7] |                        2.8 [2.2–100.0] |     10.3 [9.7–65.2] |    12.9 [11.1–65.2] |                   0 [0–4892] |                       0 [0–4329] |       4.9 [3.9–5.3] | 2173 [1751–2808] |
| Markdown Display  | 15.49 [15.19–23.50] |  6.46 [6.22–9.61] |           43.7 [42.2–47.5] |                       69.6 [61.4–81.4] |    60.7 [36.6–92.8] |   83.9 [42.6–116.2] |                  87 [71–163] |                          0 [0–3] |       4.5 [4.2–5.0] | 3475 [3067–4786] |
| Marked            | 18.93 [14.98–21.97] |  8.35 [6.55–9.75] |           89.7 [80.6–91.5] |                       74.2 [51.7–77.0] |    69.6 [49.5–82.5] |    80.0 [65.0–95.2] |                 123 [85–155] |                          0 [0–0] |       4.4 [4.2–4.8] | 4073 [3383–5178] |
| Markdown Renderer | 20.13 [10.09–22.26] | 9.39 [4.92–10.55] |           72.3 [39.4–88.6] |                       71.8 [61.1–85.1] |   86.6 [58.1–113.0] |  125.9 [76.3–228.0] |                    0 [0–337] |                          0 [0–0] |       4.7 [4.5–4.7] | 4274 [2233–4766] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |      Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | ------------------: |
| RN Streamdown     | 2.08×* [1.32–2.44] | 1.47×* [1.05–1.66] |   2.60×* [2.11–2.85] |                 0.46×* [0.20–0.95] |  0.63×* [0.59–0.98] |
| Enriched Markdown | 1.69×* [1.08–2.28] |  1.34× [0.03–1.59] |    1.51× [0.84–1.77] |                 0.36× [0.23–12.60] |   0.67× [0.57–3.38] |
| Markdown Display  | 2.44×* [1.91–3.71] | 3.80×* [2.99–5.65] |   3.74×* [3.44–4.06] |                8.77×* [5.83–17.93] |  4.34×* [2.21–7.01] |
| Marked            | 2.99×* [2.32–3.49] | 4.91×* [3.46–5.74] |   7.67×* [6.90–8.06] |                9.35×* [5.89–19.11] |  4.14×* [3.36–6.39] |
| Markdown Renderer | 3.18×* [1.56–3.71] | 5.52×* [2.60–6.21] |   6.18×* [3.37–7.57] |                9.05×* [6.01–18.49] | 6.52×* [3.95–11.81] |

### ai-feature-matrix: Math, code and tables: every subset and ordering

30 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |         App CPU (s) |   UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ------------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |  10.64 [8.37–11.21] |    2.64 [2.30–2.86] |           14.2 [13.3–14.7] |                         8.0 [3.4–11.2] |    12.9 [11.9–14.3] |    18.5 [16.8–23.9] |                    48 [0–61] |                         0 [0–37] |       6.4 [4.7–6.4] | 1688 [1304–2368] |
| RN Streamdown     | 21.72 [15.47–23.36] |    2.98 [2.79–3.67] |           39.1 [34.2–44.0] |                          5.9 [3.8–6.7] |    10.7 [10.3–11.0] |    16.0 [12.0–16.5] |                  98 [0–1113] |                        0 [0–288] |       5.8 [5.6–6.4] | 3417 [2786–4053] |
| Enriched Markdown | 26.07 [13.72–26.76] |    4.21 [2.72–5.01] |           26.4 [24.6–29.5] |                          2.6 [1.9–4.6] |     10.3 [9.9–11.0] |    11.6 [10.9–13.3] |                   71 [70–80] |                          0 [0–0] |       7.7 [4.7–8.0] | 3267 [2778–4530] |
| Markdown Display  | 33.40 [21.30–42.24] |  13.73 [9.15–17.11] |           60.0 [52.9–67.0] |                       57.8 [52.1–72.5] |  130.3 [83.2–144.6] | 193.6 [105.6–275.1] |                  323 [0–464] |                        0 [0–307] |       5.4 [4.7–7.4] | 5014 [3945–7464] |
| Marked            | 32.13 [22.95–48.61] | 14.63 [10.60–22.38] |         109.8 [96.7–164.1] |                       45.8 [44.1–60.2] | 134.2 [127.8–172.2] | 306.6 [252.0–637.5] |                   0 [0–3124] |                       0 [0–3012] |       5.5 [4.7–6.2] | 5751 [4294–7884] |
| Markdown Renderer | 36.50 [21.91–43.86] | 17.62 [10.77–21.25] |        160.0 [121.2–203.5] |                       55.4 [47.1–68.3] | 166.4 [111.9–233.9] | 483.7 [463.5–918.2] |               635 [138–4421] |                       0 [0–4267] |       6.1 [5.4–6.9] | 6257 [3586–7525] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |        Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | --------------------: |
| RN Streamdown     | 2.04×* [1.45–2.59] | 1.13×* [1.02–1.45] |   2.75×* [2.41–3.17] |                  0.74× [0.47–1.75] |    0.86×* [0.63–0.95] |
| Enriched Markdown | 2.45×* [1.29–3.11] | 1.60×* [1.03–1.99] |   1.86×* [1.73–2.07] |                 0.32×* [0.23–0.75] |    0.63×* [0.48–0.72] |
| Markdown Display  | 3.14×* [2.00–4.12] | 5.21×* [3.37–6.49] |   4.22×* [3.72–4.72] |                7.24×* [5.15–16.99] |  10.46×* [5.57–14.86] |
| Marked            | 3.02×* [2.12–4.57] | 5.55×* [3.88–8.49] |  7.72×* [6.80–11.54] |                5.74×* [4.09–13.48] | 16.56×* [12.81–34.44] |
| Markdown Renderer | 3.43×* [2.06–4.36] | 6.69×* [4.09–8.42] | 11.25×* [8.53–14.31] |                6.95×* [4.94–16.30] | 26.13×* [20.21–49.60] |

### ai-concurrent-5: Five AI replies streaming at once, including the feature matrix

30 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |         App CPU (s) |   UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) |   Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ------------------: | -------------------------: | -------------------------------------: | ------------------: | --------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    | 23.33 [15.75–32.87] |    5.29 [3.06–5.66] |           23.4 [22.2–29.3] |                          2.3 [0.8–3.8] |    10.9 [10.2–11.5] |      14.0 [12.1–16.8] |                     0 [0–49] |                          0 [0–0] |    24.8 [23.9–27.3] |   953 [624–1266] |
| RN Streamdown     | 48.86 [33.29–54.53] |   7.37 [6.60–10.16] |           72.4 [57.8–77.7] |                          2.2 [1.2–8.0] |     10.4 [9.5–16.0] |      14.1 [11.6–20.3] |                   0 [0–3512] |                       0 [0–1504] |    24.0 [22.5–25.6] | 2035 [1478–2341] |
| Enriched Markdown | 44.76 [26.13–55.13] |   9.58 [6.22–12.20] |           37.7 [36.0–39.8] |                         4.4 [1.2–11.1] |    11.1 [10.3–16.5] |      16.7 [12.1–22.3] |                    73 [0–99] |                          0 [0–0] |    25.7 [24.0–26.5] | 1799 [1015–2128] |
| Markdown Display  | 53.17 [46.00–60.97] | 20.89 [17.84–23.48] |           84.5 [77.4–88.6] |                       64.1 [61.2–76.9] |  115.0 [73.0–147.6] |    159.1 [95.4–246.5] |                122 [122–384] |                        0 [0–233] |    25.2 [23.4–27.0] | 1970 [1755–2607] |
| Marked            | 62.87 [41.55–67.08] | 26.52 [17.53–28.63] |         100.6 [95.6–104.9] |                       76.5 [59.6–80.0] |  131.2 [94.5–159.5] |   196.3 [162.9–245.6] |                  65 [52–117] |                          0 [0–0] |    25.9 [23.7–26.2] | 2401 [1605–2794] |
| Markdown Renderer | 63.43 [51.89–64.88] | 29.35 [24.25–29.81] |        167.5 [152.0–190.9] |                       54.0 [34.9–71.4] | 308.0 [134.9–635.8] | 1002.3 [385.5–1534.0] |                 300 [0–2337] |                     173 [0–1472] |    24.8 [24.5–26.8] | 2521 [1998–2633] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |         Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | ---------------------: |
| RN Streamdown     | 2.09×* [1.24–3.10] | 1.39×* [1.25–2.41] |   3.09×* [2.36–3.35] |                  0.94× [0.48–4.55] |      1.01× [0.72–1.45] |
| Enriched Markdown |  1.92× [0.98–2.84] | 1.81×* [1.18–3.13] |   1.61×* [1.28–1.72] |                  1.87× [0.49–6.56] |      1.19× [0.87–1.62] |
| Markdown Display  | 2.28×* [1.50–3.38] | 3.95×* [3.35–6.83] |   3.61×* [2.87–3.82] |              27.39×* [16.87–76.23] |   11.34×* [6.65–17.61] |
| Marked            | 2.69×* [1.53–3.99] | 5.02×* [3.32–8.67] |   4.30×* [3.43–4.54] |              32.70×* [17.96–91.03] |  14.00×* [11.48–17.51] |
| Markdown Renderer | 2.72×* [1.65–4.03] | 5.55×* [4.53–9.59] |   7.16×* [5.70–8.24] |              23.07×* [10.75–64.20] | 71.46×* [27.48–109.37] |

### ai-huge-single: One huge AI reply (60 points, mixed features)

6 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           | App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       14.72 |              4.19 |                       12.9 |                                    4.4 |                11.1 |                11.8 |                            0 |                                0 |                15.1 |              974 |
| RN Streamdown     |       32.32 |              5.68 |                       68.1 |                                    2.8 |                10.7 |                11.7 |                          165 |                                0 |                14.8 |             2183 |
| Enriched Markdown |       34.78 |              7.30 |                       26.1 |                                    3.5 |                10.9 |                12.0 |                           94 |                                0 |                15.4 |             2264 |
| Markdown Display  |       64.60 |             29.41 |                       85.5 |                                   79.8 |               193.0 |               262.8 |                          485 |                                0 |                16.7 |             3873 |
| Marked            |       70.27 |             32.22 |                      108.8 |                                   83.7 |               223.7 |               406.8 |                          575 |                                0 |                16.8 |             4172 |
| Markdown Renderer |       73.42 |             34.88 |                      157.8 |                                   77.6 |               307.0 |               738.9 |                            0 |                                0 |                15.4 |             4760 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | UI thread CPU | Peak memory increase | Janky frames (app deadline missed) | Frame time p99 |
| ----------------- | ------: | ------------: | -------------------: | ---------------------------------: | -------------: |
| RN Streamdown     |   2.20× |         1.36× |                5.28× |                              0.65× |          1.00× |
| Enriched Markdown |   2.36× |         1.74× |                2.02× |                              0.80× |          1.02× |
| Markdown Display  |   4.39× |         7.02× |                6.63× |                             18.29× |         22.30× |
| Marked            |   4.77× |         7.69× |                8.45× |                             19.18× |         34.51× |
| Markdown Renderer |   4.99× |         8.32× |               12.25× |                             17.79× |         62.69× |

### ai-huge-concurrent-5: Five huge AI replies (60 points each, mixed features)

6 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           | App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       36.40 |              5.82 |                       39.1 |                                    0.7 |                 9.8 |                12.4 |                            0 |                                0 |                72.8 |              500 |
| RN Streamdown     |       93.68 |             13.39 |                      145.6 |                                   17.9 |                18.3 |                21.2 |                          265 |                                0 |                78.4 |             1195 |
| Enriched Markdown |       74.92 |             12.56 |                       76.5 |                                   12.1 |                16.5 |                21.5 |                           26 |                                0 |                77.6 |              965 |
| Markdown Display  |       99.65 |             41.16 |                      179.4 |                                   71.1 |               535.1 |               908.1 |                         1431 |                               19 |                71.4 |             1396 |
| Marked            |       93.82 |             37.81 |                      120.6 |                                   72.3 |               281.1 |               377.0 |                           38 |                                0 |                77.2 |             1215 |
| Markdown Renderer |      113.09 |             53.66 |                      489.6 |                                   71.7 |               306.4 |              1794.1 |                        13461 |                            11721 |                77.8 |             1454 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | UI thread CPU | Peak memory increase | Janky frames (app deadline missed) | Frame time p99 |
| ----------------- | ------: | ------------: | -------------------: | ---------------------------------: | -------------: |
| RN Streamdown     |   2.57× |         2.30× |                3.72× |                             26.74× |          1.71× |
| Enriched Markdown |   2.06× |         2.16× |                1.96× |                             18.03× |          1.73× |
| Markdown Display  |   2.74× |         7.08× |                4.59× |                            106.29× |         73.21× |
| Marked            |   2.58× |         6.50× |                3.08× |                            108.08× |         30.39× |
| Markdown Renderer |   3.11× |         9.22× |               12.52× |                            107.20× |        144.63× |

## Code colors on (library defaults), default streaming animations on

### ai-single: One AI reply (prose, table, TypeScript)

15 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |        App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | -----------------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |   8.07 [5.42–9.82] |  2.11 [1.36–2.35] |           15.1 [14.8–15.7] |                          0.9 [0.5–1.1] |    11.1 [10.3–14.4] |    13.9 [12.7–15.5] |                      0 [0–6] |                          0 [0–0] |       4.5 [4.3–4.9] | 1781 [1269–2207] |
| RN Streamdown     | 12.09 [8.32–13.53] |  2.97 [2.41–3.69] |           28.9 [22.9–30.8] |                          0.8 [0.3–1.0] |      8.8 [7.8–10.1] |     11.1 [9.5–12.6] |                 180 [66–348] |                        0 [0–196] |       4.8 [4.5–5.0] | 2563 [1820–2732] |
| Enriched Markdown |   9.54 [9.29–9.86] |  3.08 [2.96–3.23] |           16.3 [14.8–17.4] |                          0.9 [0.4–2.5] |       8.2 [7.7–9.3] |    10.9 [10.3–15.8] |                     0 [0–29] |                          0 [0–0] |       4.6 [4.3–4.9] | 2117 [1954–2142] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |     Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | -----------------: |
| RN Streamdown     | 1.50×* [1.03–2.23] | 1.41×* [1.14–2.22] |   1.92×* [1.52–2.04] |                  0.86× [0.27–1.51] | 0.80×* [0.68–0.91] |
| Enriched Markdown |  1.18× [0.97–1.76] | 1.46×* [1.31–2.26] |    1.08× [0.98–1.15] |                  0.93× [0.38–2.63] |  0.79× [0.71–1.14] |

### ai-code-heavy: One code-heavy AI reply (14 code blocks)

15 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |        App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | -----------------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |   8.09 [5.65–9.42] |  2.04 [1.50–2.80] |           16.9 [14.2–17.3] |                          2.4 [0.7–2.8] |    14.2 [12.5–17.4] |    17.6 [15.1–24.5] |                    0 [0–393] |                        0 [0–233] |       4.6 [4.4–5.0] | 1776 [1245–1976] |
| RN Streamdown     | 14.70 [8.78–16.96] |  3.29 [2.09–3.50] |           33.7 [29.2–38.7] |                          3.0 [1.5–3.9] |     10.5 [9.4–12.0] |    16.1 [11.9–18.8] |                      0 [0–0] |                          0 [0–0] |       4.5 [4.3–5.5] | 2764 [2020–3842] |
| Enriched Markdown | 11.06 [9.51–15.16] |  3.30 [2.68–4.31] |           19.4 [18.3–19.9] |                          3.5 [0.9–5.9] |     11.3 [9.9–12.6] |    16.6 [12.3–18.2] |                     0 [0–37] |                          0 [0–0] |       4.3 [4.1–5.4] | 2637 [2190–2833] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |    Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | ----------------: |
| RN Streamdown     | 1.82×* [1.08–2.60] | 1.61×* [1.02–2.19] |   2.00×* [1.73–2.42] |                  1.27× [0.57–4.03] | 0.91× [0.58–1.08] |
| Enriched Markdown | 1.37×* [1.09–1.96] | 1.61×* [1.16–2.20] |   1.15×* [1.09–1.37] |                  1.47× [0.40–4.67] | 0.94× [0.60–1.10] |

### ai-feature-matrix: Math, code and tables: every subset and ordering

15 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |         App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    | 10.99 [10.08–12.29] |  2.73 [2.36–3.30] |           18.4 [16.7–18.7] |                          1.4 [0.4–5.1] |    12.2 [10.8–15.3] |    16.9 [14.3–18.7] |                    37 [0–44] |                          0 [0–0] |       5.6 [4.7–5.9] | 2090 [1920–2128] |
| RN Streamdown     | 21.41 [20.68–28.54] |  4.02 [3.25–5.04] |           40.1 [38.9–57.2] |                          1.9 [1.2–6.6] |     10.2 [9.2–11.7] |    13.3 [11.7–18.1] |                   68 [0–833] |                          0 [0–0] |       6.3 [5.8–8.7] | 3461 [2866–4451] |
| Enriched Markdown | 16.96 [13.86–32.75] |  3.36 [2.71–5.09] |           29.6 [25.5–33.2] |                          2.2 [1.7–6.1] |     10.2 [9.7–11.9] |    15.9 [12.4–19.0] |                    53 [0–71] |                          0 [0–0] |       5.4 [4.9–7.6] | 3470 [2571–4692] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |    Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | ----------------: |
| RN Streamdown     | 1.95×* [1.74–2.70] | 1.47×* [1.18–1.85] |   2.19×* [2.09–3.12] |                  1.35× [0.38–5.53] | 0.79× [0.69–1.18] |
| Enriched Markdown | 1.54×* [1.24–2.98] |  1.23× [0.98–2.04] |   1.61×* [1.39–1.84] |                  1.55× [0.44–5.44] | 0.94× [0.73–1.14] |

### ai-concurrent-5: Five AI replies streaming at once, including the feature matrix

15 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |         App CPU (s) |  UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | -----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    | 25.66 [24.20–42.07] |   5.55 [5.42–6.71] |           29.7 [26.9–39.3] |                          0.8 [0.4–1.0] |    14.3 [10.9–14.8] |    15.9 [14.3–20.4] |                    48 [0–60] |                          0 [0–0] |    25.1 [23.5–25.7] |  1092 [943–1676] |
| RN Streamdown     | 51.19 [42.59–53.26] | 10.36 [9.11–11.20] |           69.1 [66.4–83.5] |                          4.6 [1.4–6.4] |    12.9 [10.4–16.2] |    19.2 [13.2–21.4] |                   71 [0–847] |                        0 [0–402] |    24.7 [24.2–26.6] | 2003 [1758–2084] |
| Enriched Markdown | 44.59 [39.29–60.33] | 10.53 [8.61–13.55] |           42.3 [39.4–44.9] |                          2.8 [1.0–5.9] |    12.1 [10.6–13.9] |    15.7 [12.8–17.8] |                    63 [0–85] |                          0 [0–0] |    25.5 [23.9–26.4] | 1866 [1516–2458] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |      UI thread CPU | Peak memory increase | Janky frames (app deadline missed) |    Frame time p99 |
| ----------------- | -----------------: | -----------------: | -------------------: | ---------------------------------: | ----------------: |
| RN Streamdown     | 1.99×* [1.22–2.12] | 1.86×* [1.52–2.02] |   2.33×* [1.76–2.82] |                6.13×* [1.80–10.76] | 1.21× [0.80–1.41] |
| Enriched Markdown | 1.74×* [1.06–2.35] | 1.90×* [1.51–2.44] |   1.43×* [1.08–1.59] |                 3.69×* [1.31–7.77] | 0.98× [0.76–1.19] |

### ai-huge-single: One huge AI reply (60 points, mixed features)

3 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           | App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       33.17 |              6.48 |                       21.5 |                                    3.2 |                14.5 |                15.0 |                            0 |                                0 |                15.9 |             2088 |
| RN Streamdown     |       61.89 |              9.31 |                      125.2 |                                    0.9 |                10.4 |                12.1 |                            0 |                                0 |                16.5 |             3742 |
| Enriched Markdown |       56.66 |              9.58 |                       29.9 |                                    1.3 |                10.4 |                12.1 |                           55 |                                0 |                14.8 |             3818 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | UI thread CPU | Peak memory increase | Janky frames (app deadline missed) | Frame time p99 |
| ----------------- | ------: | ------------: | -------------------: | ---------------------------------: | -------------: |
| RN Streamdown     |   1.87× |         1.44× |                5.81× |                              0.27× |          0.81× |
| Enriched Markdown |   1.71× |         1.48× |                1.39× |                              0.41× |          0.81× |

### ai-huge-concurrent-5: Five huge AI replies (60 points each, mixed features)

3 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           | App CPU (s) | UI thread CPU (s) | Peak memory increase (MiB) | Janky frames (app deadline missed) (%) | Frame time p95 (ms) | Frame time p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ----------------: | -------------------------: | -------------------------------------: | ------------------: | ------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       77.36 |              9.36 |                       60.9 |                                    0.4 |                13.7 |                14.5 |                           54 |                                0 |                76.3 |             1014 |
| RN Streamdown     |       88.07 |             11.11 |                      156.6 |                                    0.8 |                11.1 |                14.2 |                            0 |                                0 |                70.4 |             1252 |
| Enriched Markdown |      108.08 |             26.97 |                       77.1 |                                    9.5 |                14.3 |                18.4 |                           84 |                                0 |                78.4 |             1379 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | UI thread CPU | Peak memory increase | Janky frames (app deadline missed) | Frame time p99 |
| ----------------- | ------: | ------------: | -------------------: | ---------------------------------: | -------------: |
| RN Streamdown     |   1.14× |         1.19× |                2.57× |                              2.20× |          0.98× |
| Enriched Markdown |   1.40× |         2.88× |                1.27× |                             25.59× |          1.27× |

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
- **Settle.** Time from the last delivered chunk to the last completed frame the app drew, from the trace. New runs also split last text to app finish receipt from finish receipt to last frame. Finish receipt is separate from host done, which can wait for provider usage metadata. Raw runs contain per-thread boundaries and scheduler/frame attribution before and after finish. Missing boundaries in older runs remain unknown.
- **Animation profiles.** `highlighted` and `plain` disable streaming animations. `highlighted-animated` and `plain-animated` enable each supported library's default effect and include only Downstream, Streamdown and Enriched. These effects differ: Enriched also changes incomplete block presentation and defers code highlighting. Compare profiles with the same replay recording; live replies differ. The 150 ms renderer-tail target applies to animation-off runs. Open frames or app drawing at/after the window conservatively mark the tail as censored. Later frames can include benchmark activity; inspect the retained trace before attributing them to the renderer. Use `--settle-ms` to extend the same observation budget for all profiles.
- **JS lateness.** How late the app delivered each chunk against its schedule, a proxy for a busy JS thread.
- **Window.** From the moment streaming starts until 2 s after stream done is received (after the final chunk in replay runs). App start-up and mounting are outside it.
- **Order and noise.** 5 measured blocks plus one warmup block; every block runs each library and workload once in a seeded random order (seed 1). Before each run the app is force-stopped, background apps are killed and the harness waits until the thermal status is 0 and the battery is at most 35 °C. App code is AOT-compiled (`cmd package compile -m speed`). The screen stays on at fixed brightness with the refresh rate pinned.
- **Statistics.** Medians of per-run values, percentile bootstrap 95% intervals (4,000 resamples). Ratios are bootstrapped from both groups. Mann-Whitney U is two-sided with a normal approximation; no multiple-comparison correction, so read single stars with care.
- **Limits.** One phone model. The common app contains every library, so memory is not a per-package size. Clock speeds are not locked (needs root). Markdown output differs between libraries (see Features); equal input is not equal rendering.

Raw runs, traces and the full statistics are in `results/bench-20261015T162349` (summary.json). Build: `highlighted 34348de3d106, highlighted-animated 34348de3d106`, revision `5ea121895f` with local changes.
