# iOS simulator streaming benchmark

iPhone 18 Pro simulator · iOS 27.0 · 60 Hz · Mac17,3 (Apple M5), macOS 27.0.1 · arm64 Hermes release · 2026-10-18. 196 measured runs, 36 warmups excluded. Every number is the median of 5 cold-process runs, with its 95% bootstrap confidence interval in brackets. Lower is better everywhere.

A simulator runs the iOS frameworks on the Mac’s CPU and GPU, so absolute numbers are not iPhone numbers. The libraries share the same app, input and Mac, so their ratios are the useful part.

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

Live: every run asks `gpt-6-luna` (reasoning medium) for fresh replies through the Vercel AI SDK `streamText` on the Mac, and each text delta reaches the simulator over loopback the moment it arrives. Replies differ from run to run, so "CPU per KiB" normalizes CPU by the text each run actually streamed.

## Code colors on (library defaults), streaming animations off

### ai-single: One AI reply (prose, table, TypeScript)

| Library           |      App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ---------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    | 2.33 [2.22–2.55] |    0.95 [0.91–1.08] |              65.7 [34.5–67.5] |                                     0.0 [0.0–0.3] |           0.0 [0.0–1.6] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                225 [153–262] |                       76 [70–82] |       4.5 [4.3–4.9] |    539 [457–553] |
| RN Streamdown     | 3.29 [2.98–4.14] |    0.86 [0.70–0.93] |              73.4 [70.7–80.7] |                                     0.3 [0.0–0.7] |           3.2 [0.0–5.9] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   34 [29–43] |                         0 [0–14] |       4.5 [4.3–5.2] |    753 [686–798] |
| Enriched Markdown | 3.43 [3.13–3.67] |    1.28 [1.15–1.36] |              57.6 [51.1–60.1] |                                     0.2 [0.0–0.4] |           1.7 [0.0–3.4] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   32 [25–36] |                          0 [0–0] |       4.5 [4.4–4.7] |    756 [704–787] |
| Markdown Display  | 4.96 [4.77–5.73] |    1.94 [1.86–2.26] |              34.4 [33.5–39.2] |                                     0.0 [0.0–0.2] |           0.0 [0.0–1.4] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   31 [25–41] |                          0 [0–0] |       4.5 [4.2–5.1] |  1125 [972–1172] |
| Marked            | 5.10 [4.87–6.24] |    2.56 [2.47–3.42] |              82.0 [77.6–89.3] |                                     0.0 [0.0–0.5] |           0.0 [0.0–4.3] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   26 [23–38] |                          0 [0–0] |       4.6 [4.5–5.0] | 1081 [1034–1242] |
| Markdown Renderer | 4.26 [3.37–4.68] |    2.58 [1.88–2.84] |              69.2 [66.8–83.8] |                                     0.2 [0.0–0.8] |           1.5 [0.0–4.3] |        16.7 [16.7–16.7] |        16.7 [16.7–17.0] |                   33 [28–37] |                          0 [0–0] |       4.6 [4.4–5.1] |    939 [666–967] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | -----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 1.41×* [1.27–1.77] | 0.90×* [0.73–0.97] |      1.12×* [1.05–2.13] |                                             – |  1.00× [1.00–1.00] |
| Enriched Markdown | 1.47×* [1.32–1.58] | 1.34×* [1.18–1.44] |       0.88× [0.78–1.67] |                                             – |  1.00× [1.00–1.00] |
| Markdown Display  | 2.12×* [1.94–2.45] | 2.03×* [1.80–2.37] |      0.52×* [0.50–1.00] |                                             – |  1.00× [1.00–1.00] |
| Marked            | 2.19×* [2.00–2.67] | 2.68×* [2.37–3.58] |      1.25×* [1.17–2.38] |                                             – |  1.00× [1.00–1.00] |
| Markdown Renderer | 1.83×* [1.44–2.01] | 2.71×* [1.96–2.98] |      1.05×* [1.01–2.07] |                                             – |  1.00× [1.00–1.02] |

### ai-code-heavy: One code-heavy AI reply (14 code blocks)

| Library           |      App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ---------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    | 2.48 [2.28–3.08] |    1.05 [0.96–1.26] |              53.0 [44.7–55.1] |                                     0.0 [0.0–0.5] |           0.0 [0.0–6.3] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                 245 [58–667] |                        73 [0–83] |       4.4 [3.8–5.0] |    581 [541–659] |
| RN Streamdown     | 3.44 [3.25–4.78] |    0.72 [0.65–0.86] |              81.9 [75.0–88.4] |                                     0.2 [0.2–0.6] |           2.5 [2.1–4.4] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   38 [10–61] |                          0 [0–0] |       4.7 [4.3–5.3] |    766 [727–902] |
| Enriched Markdown | 3.42 [3.13–4.63] |    0.85 [0.81–1.09] |              62.4 [55.8–71.5] |                                     0.0 [0.0–0.2] |           2.5 [1.7–3.9] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   33 [10–38] |                          0 [0–0] |       4.5 [4.0–5.6] |    807 [715–891] |
| Markdown Display  | 6.39 [5.89–8.18] |    2.83 [2.51–3.63] |              44.1 [40.5–49.8] |                                     0.0 [0.0–0.0] |           0.0 [0.0–0.0] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   32 [26–37] |                          0 [0–0] |       4.5 [4.0–4.9] | 1490 [1315–1721] |
| Marked            | 6.66 [4.83–7.23] |    3.26 [2.26–3.60] |              73.5 [70.4–77.1] |                                     0.5 [0.0–0.7] |           4.2 [0.0–5.3] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   36 [28–46] |                          0 [0–0] |       4.4 [4.1–4.8] | 1501 [1186–1630] |
| Markdown Renderer | 6.57 [4.96–7.55] |    3.99 [2.99–4.92] |           129.2 [117.3–136.8] |                                     1.5 [0.3–3.2] |         13.2 [2.3–22.9] |        16.7 [16.7–16.7] |        33.3 [16.7–33.3] |                   28 [23–37] |                          0 [0–0] |       4.6 [4.2–5.4] | 1387 [1109–1558] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | -----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 1.39×* [1.12–1.93] | 0.68×* [0.57–0.82] |      1.55×* [1.42–1.85] |                                             – |  1.00× [1.00–1.00] |
| Enriched Markdown | 1.38×* [1.11–1.87] |  0.81× [0.67–1.04] |      1.18×* [1.02–1.52] |                                             – |  1.00× [1.00–1.00] |
| Markdown Display  | 2.58×* [2.08–3.30] | 2.68×* [2.24–3.44] |       0.83× [0.76–1.01] |                                             – |  1.00× [1.00–1.00] |
| Marked            | 2.69×* [1.95–2.98] | 3.09×* [2.14–3.58] |      1.39×* [1.32–1.68] |                                             – |  1.00× [1.00–1.00] |
| Markdown Renderer | 2.65×* [2.00–3.05] | 3.79×* [2.84–4.67] |      2.44×* [2.21–2.91] |                                             – | 2.00×* [1.00–2.00] |

### ai-feature-matrix: Math, code and tables: every subset and ordering

| Library           |         App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |    3.91 [3.18–4.82] |    1.69 [1.38–2.09] |             68.3 [54.5–102.5] |                                     0.0 [0.0–0.0] |           0.0 [0.0–0.0] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                455 [132–702] |                       76 [68–83] |       5.9 [4.8–7.2] |    671 [616–708] |
| RN Streamdown     |   7.06 [5.50–11.75] |    0.78 [0.72–0.88] |           121.3 [116.3–144.3] |                                     0.4 [0.0–0.6] |           2.1 [2.0–3.2] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                  42 [35–939] |                        0 [0–390] |       6.1 [5.8–8.1] |  1155 [951–1457] |
| Enriched Markdown |    6.13 [5.40–6.53] |    1.06 [0.93–1.23] |              87.4 [82.6–98.4] |                                     0.1 [0.0–0.4] |           2.0 [0.9–2.9] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   33 [25–36] |                          0 [0–0] |       5.8 [4.9–6.8] |  1035 [955–1109] |
| Markdown Display  |  12.94 [8.76–18.14] |    6.24 [3.92–8.70] |              52.1 [43.9–70.9] |                                    9.1 [5.4–16.0] |       91.0 [44.5–165.1] |        37.9 [33.3–50.8] |        50.4 [44.7–56.0] |                   47 [31–86] |                          0 [0–0] |       6.1 [4.5–6.6] | 2120 [1945–2731] |
| Marked            | 15.40 [14.80–17.04] |    8.02 [7.70–9.05] |              84.6 [76.7–88.2] |                                  18.5 [14.1–21.3] |     185.0 [148.3–188.9] |        50.7 [47.5–50.8] |        57.6 [55.4–60.7] |                   73 [48–88] |                          0 [0–0] |       6.3 [5.6–6.8] | 2623 [2336–2673] |
| Markdown Renderer | 13.21 [12.10–17.73] |   7.71 [6.99–10.63] |            102.1 [96.7–106.9] |                                  17.4 [14.1–22.3] |     182.8 [146.6–204.3] |        50.2 [47.0–52.4] |        68.0 [65.1–69.8] |                   57 [52–81] |                          0 [0–0] |       6.1 [6.0–6.8] | 2197 [2001–2639] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | -----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 1.80×* [1.41–3.01] | 0.46×* [0.37–0.57] |      1.78×* [1.18–2.23] |                                             – |  1.00× [1.00–1.00] |
| Enriched Markdown | 1.57×* [1.27–1.94] | 0.63×* [0.51–0.83] |       1.28× [0.85–1.60] |                                             – |  1.00× [1.00–1.00] |
| Markdown Display  | 3.31×* [2.24–4.64] | 3.70×* [2.32–5.20] |       0.76× [0.51–1.10] |                                             – | 3.02×* [2.68–3.36] |
| Marked            | 3.94×* [3.19–5.05] | 4.76×* [3.84–6.14] |       1.24× [0.83–1.55] |                                             – | 3.46×* [3.32–3.64] |
| Markdown Renderer | 3.38×* [2.74–4.93] | 4.57×* [3.69–6.69] |       1.49× [1.00–1.87] |                                             – | 4.08×* [3.91–4.19] |

### ai-concurrent-5: Five AI replies streaming at once, including the feature matrix

| Library           |         App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |  10.74 [9.68–12.14] |    3.63 [3.29–3.97] |           247.3 [195.9–260.0] |                                     0.0 [0.0–0.0] |           0.0 [0.0–0.0] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                693 [257–748] |                       74 [60–78] |    25.4 [23.9–26.6] |    435 [382–456] |
| RN Streamdown     | 17.32 [14.22–20.56] |    3.20 [2.65–3.70] |           343.8 [337.9–350.3] |                                     0.1 [0.0–0.1] |           1.4 [0.7–1.7] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   43 [38–58] |                          0 [0–0] |    24.2 [23.7–24.8] |    715 [598–833] |
| Enriched Markdown | 14.94 [13.25–15.50] |    3.76 [3.49–4.72] |           287.5 [269.6–297.1] |                                     0.0 [0.0–0.1] |           1.2 [0.0–2.1] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   35 [24–43] |                          0 [0–0] |    24.4 [23.7–25.6] |    601 [544–631] |
| Markdown Display  | 28.32 [25.09–31.64] | 12.92 [11.45–14.74] |           131.3 [119.5–164.3] |                                  24.0 [17.2–28.1] |     250.6 [200.3–271.1] |        53.6 [50.5–56.4] |        63.5 [57.8–68.7] |                   57 [35–71] |                          0 [0–0] |    24.4 [23.8–26.0] | 1144 [1053–1298] |
| Marked            | 24.22 [23.09–29.09] | 12.14 [11.23–14.47] |           130.5 [114.3–145.6] |                                    7.3 [6.4–13.0] |       98.1 [59.8–138.0] |        39.6 [33.3–44.8] |        57.4 [52.0–60.0] |                   26 [25–37] |                          0 [0–0] |    25.1 [23.4–26.9] |   953 [921–1242] |
| Markdown Renderer | 24.99 [22.84–26.25] | 14.07 [12.60–15.04] |           163.7 [158.1–167.3] |                                  24.2 [22.0–28.5] |     294.8 [249.8–298.3] |        57.9 [54.7–66.8] |        79.6 [69.0–97.1] |                   51 [26–79] |                          0 [0–0] |    24.8 [22.5–25.5] |  1014 [969–1125] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | -----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 1.61×* [1.30–1.91] |  0.88× [0.72–1.02] |      1.39×* [1.32–1.75] |                                             – |  1.00× [1.00–1.00] |
| Enriched Markdown | 1.39×* [1.16–1.54] |  1.04× [0.94–1.33] |      1.16×* [1.06–1.47] |                                             – |  1.00× [1.00–1.00] |
| Markdown Display  | 2.64×* [2.26–2.95] | 3.56×* [3.09–4.06] |      0.53×* [0.47–0.67] |                                             – | 3.81×* [3.47–4.12] |
| Marked            | 2.25×* [1.99–2.71] | 3.35×* [3.03–3.99] |      0.53×* [0.46–0.67] |                                             – | 3.44×* [3.12–3.60] |
| Markdown Renderer | 2.33×* [2.00–2.58] | 3.88×* [3.40–4.36] |      0.66×* [0.63–0.84] |                                             – | 4.78×* [4.14–5.82] |

### ai-huge-single: One huge AI reply (60 points, mixed features)

Marked failed: RangeError: Property storage exceeds 196607 properties

| Library           | App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       13.01 |                5.69 |                         126.8 |                                               0.0 |                     0.0 |                    16.7 |                    16.7 |                          289 |                               75 |                17.1 |              760 |
| RN Streamdown     |       23.35 |                1.80 |                         259.6 |                                               0.0 |                     0.0 |                    16.7 |                    16.7 |                          106 |                                0 |                17.3 |             1351 |
| Enriched Markdown |       22.11 |                4.50 |                         163.5 |                                               0.0 |                     0.7 |                    16.7 |                    16.7 |                           38 |                                0 |                16.5 |             1340 |
| Markdown Display  |       42.45 |               20.86 |                         126.5 |                                              22.4 |                   259.6 |                    59.1 |                    69.8 |                           95 |                                0 |                15.0 |             2822 |
| Markdown Renderer |       44.61 |               23.97 |                         113.1 |                                              28.2 |                   306.0 |                    59.7 |                    82.0 |                          103 |                                0 |                16.1 |             2772 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | ------: | --------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     |   1.79× |           0.32× |                   2.05× |                                             – |              1.00× |
| Enriched Markdown |   1.70× |           0.79× |                   1.29× |                                             – |              1.00× |
| Markdown Display  |   3.26× |           3.67× |                   1.00× |                                             – |              4.19× |
| Markdown Renderer |   3.43× |           4.21× |                   0.89× |                                             – |              4.92× |

### ai-huge-concurrent-5: Five huge AI replies (60 points each, mixed features)

Marked failed: RangeError: Property storage exceeds 196607 properties

| Library           | App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       25.60 |                8.52 |                         412.7 |                                               0.0 |                     0.4 |                    16.7 |                    16.7 |                          908 |                               72 |                78.1 |              328 |
| RN Streamdown     |       62.12 |                4.32 |                         374.2 |                                               0.2 |                     1.0 |                    16.7 |                    16.7 |                            0 |                                0 |                78.2 |              795 |
| Enriched Markdown |       52.52 |                9.69 |                         676.0 |                                               0.0 |                     0.7 |                    16.7 |                    16.7 |                           36 |                                0 |                75.4 |              697 |
| Markdown Display  |       95.40 |               45.61 |                         384.3 |                                              50.9 |                   615.7 |                   129.1 |                   214.1 |                           84 |                                0 |                78.7 |             1211 |
| Markdown Renderer |       94.38 |               46.98 |                         377.2 |                                              50.0 |                   610.9 |                   145.8 |                   237.5 |                           92 |                                0 |                74.8 |             1261 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | ------: | --------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     |   2.43× |           0.51× |                   0.91× |                                         4.39× |              1.00× |
| Enriched Markdown |   2.05× |           1.14× |                   1.64× |                                         0.00× |              1.00× |
| Markdown Display  |   3.73× |           5.36× |                   0.93× |                                      1215.42× |             12.84× |
| Markdown Renderer |   3.69× |           5.52× |                   0.91× |                                      1192.90× |             14.25× |

## Code colors on (library defaults), default streaming animations on

### ai-single: One AI reply (prose, table, TypeScript)

| Library           |      App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ---------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    | 2.64 [2.48–3.14] |    1.57 [1.44–1.78] |              46.1 [42.9–52.7] |                                     0.0 [0.0–0.2] |           0.0 [0.0–1.9] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                551 [413–580] |                      180 [0–386] |       4.7 [4.5–4.9] |    578 [524–668] |
| RN Streamdown     | 3.10 [2.95–3.56] |    1.11 [0.89–1.22] |              74.2 [64.5–81.6] |                                     0.4 [0.2–0.7] |           3.7 [1.4–5.8] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |               237 [224–1154] |                      122 [0–772] |       4.6 [4.2–4.9] |    704 [621–774] |
| Enriched Markdown | 2.53 [2.02–2.81] |    1.21 [1.14–1.52] |              56.9 [52.3–68.0] |                                     0.2 [0.0–1.0] |           3.4 [1.8–7.5] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                 226 [28–237] |                        0 [0–132] |       4.3 [4.2–4.6] |    550 [480–649] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |           App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | ----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 1.17× [0.97–1.39] | 0.71×* [0.56–0.79] |      1.61×* [1.39–1.79] |                                             – |  1.00× [1.00–1.00] |
| Enriched Markdown | 0.96× [0.70–1.10] | 0.77×* [0.67–0.98] |      1.23×* [1.03–1.48] |                                             – |  1.00× [1.00–1.00] |

### ai-code-heavy: One code-heavy AI reply (14 code blocks)

| Library           |      App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ---------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    | 2.70 [2.18–3.04] |    1.50 [1.12–1.65] |              79.4 [54.9–90.6] |                                     0.0 [0.0–0.5] |           0.0 [0.0–5.3] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                515 [462–529] |                     349 [72–446] |       4.6 [4.2–5.2] |    577 [469–685] |
| RN Streamdown     | 2.62 [2.43–3.12] |    0.80 [0.68–0.93] |              81.1 [76.0–88.4] |                                     0.4 [0.0–0.6] |           3.2 [0.0–4.6] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                 35 [25–1071] |                        0 [0–513] |       4.6 [4.2–5.0] |    565 [552–627] |
| Enriched Markdown | 2.32 [1.70–2.52] |    1.02 [0.71–1.13] |              63.1 [56.8–66.2] |                                     0.3 [0.0–0.4] |           4.0 [2.9–6.5] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                   31 [22–56] |                          0 [0–0] |       4.4 [3.8–4.5] |    519 [454–561] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |           App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | ----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 0.97× [0.85–1.20] | 0.54×* [0.45–0.72] |       1.02× [0.87–1.48] |                                             – |  1.00× [1.00–1.00] |
| Enriched Markdown | 0.86× [0.63–1.06] | 0.68×* [0.48–0.91] |       0.79× [0.70–1.15] |                                             – |  1.00× [1.00–1.00] |

### ai-feature-matrix: Math, code and tables: every subset and ordering

1 run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.

| Library           |       App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |  4.36 [3.77–5.93] |    2.35 [2.08–2.93] |            106.7 [96.6–134.8] |                                     0.0 [0.0–0.2] |           0.0 [0.0–2.0] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |              1025 [493–1933] |                    154 [69–1360] |       5.9 [5.6–9.1] |    685 [637–823] |
| RN Streamdown     | 6.32 [5.62–12.28] |    1.04 [0.87–1.24] |            118.1 [95.5–143.2] |                                     0.4 [0.1–0.5] |           2.8 [1.8–6.3] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                261 [39–2695] |                     103 [0–2035] |       6.2 [5.2–7.4] |  1073 [856–1679] |
| Enriched Markdown |  4.43 [3.75–5.59] |    1.41 [1.12–1.65] |             83.3 [72.1–102.6] |                                     0.1 [0.0–0.7] |           3.5 [2.3–6.8] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                 230 [35–252] |                         0 [0–73] |       6.1 [4.6–7.9] |    814 [705–848] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | -----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 1.45×* [1.06–2.82] | 0.44×* [0.35–0.53] |       1.11× [0.79–1.34] |                                             – |  1.00× [1.00–1.00] |
| Enriched Markdown |  1.02× [0.75–1.39] | 0.60×* [0.47–0.78] |      0.78×* [0.60–0.96] |                                             – |  1.00× [1.00–1.00] |

### ai-concurrent-5: Five AI replies streaming at once, including the feature matrix

| Library           |         App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ------------------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |   9.78 [7.27–11.57] |    4.41 [3.11–5.39] |           286.5 [275.5–319.7] |                                     0.1 [0.0–0.2] |           0.6 [0.0–3.0] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                474 [320–740] |                      77 [71–311] |    24.7 [22.9–27.0] |    383 [317–428] |
| RN Streamdown     | 14.54 [11.99–15.82] |    2.88 [2.77–4.03] |           330.6 [299.3–352.5] |                                     0.1 [0.1–0.2] |           1.9 [1.0–3.6] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                338 [35–1210] |                      173 [0–968] |    25.1 [22.4–26.0] |    579 [515–608] |
| Enriched Markdown | 10.92 [10.11–12.15] |    4.17 [3.95–4.76] |           276.5 [262.4–305.3] |                                     0.1 [0.0–0.1] |           1.2 [0.7–2.5] |        16.7 [16.7–16.7] |        16.7 [16.7–16.7] |                  35 [28–506] |                         0 [0–87] |    25.7 [24.9–26.8] |    409 [407–489] |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           |            App CPU |    Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | -----------------: | -----------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     | 1.49×* [1.20–2.00] | 0.65×* [0.53–0.97] |       1.15× [0.99–1.23] |                             1.61× [0.32–2.74] |  1.00× [1.00–1.00] |
| Enriched Markdown |  1.12× [0.94–1.50] |  0.95× [0.77–1.34] |       0.96× [0.84–1.07] |                             1.07× [0.00–1.36] |  1.00× [1.00–1.00] |

### ai-huge-single: One huge AI reply (60 points, mixed features)

| Library           | App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       10.12 |                5.80 |                         113.5 |                                               0.0 |                     0.0 |                    16.7 |                    16.7 |                          670 |                               75 |                15.6 |              647 |
| RN Streamdown     |       17.84 |                2.99 |                         241.9 |                                               0.2 |                     1.8 |                    16.7 |                    16.7 |                          245 |                                0 |                15.6 |             1145 |
| Enriched Markdown |       12.71 |                4.11 |                         155.8 |                                               0.1 |                     1.6 |                    16.7 |                    16.7 |                          224 |                                0 |                15.5 |              821 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | ------: | --------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     |   1.76× |           0.51× |                   2.13× |                                             – |              1.00× |
| Enriched Markdown |   1.26× |           0.71× |                   1.37× |                                             – |              1.00× |

### ai-huge-concurrent-5: Five huge AI replies (60 points each, mixed features)

| Library           | App CPU (s) | Main thread CPU (s) | Peak footprint increase (MiB) | Hitched frames (main thread missed a refresh) (%) | Hitch time ratio (ms/s) | Frame interval p95 (ms) | Frame interval p99 (ms) | Settle after last chunk (ms) | Settle after finish receipt (ms) | Text streamed (KiB) | CPU per KiB (ms) |
| ----------------- | ----------: | ------------------: | ----------------------------: | ------------------------------------------------: | ----------------------: | ----------------------: | ----------------------: | ---------------------------: | -------------------------------: | ------------------: | ---------------: |
| **Downstream**    |       23.61 |               10.95 |                         492.8 |                                               0.2 |                     2.1 |                    16.7 |                    16.7 |                         1224 |                               71 |                73.7 |              320 |
| RN Streamdown     |       47.34 |                4.84 |                         545.9 |                                               0.2 |                     1.9 |                    16.7 |                    16.7 |                         2054 |                             1041 |                76.2 |              621 |
| Enriched Markdown |       41.21 |               12.00 |                         666.5 |                                               0.1 |                     1.2 |                    16.7 |                    16.7 |                          228 |                                0 |                72.7 |              567 |

Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):

| Library           | App CPU | Main thread CPU | Peak footprint increase | Hitched frames (main thread missed a refresh) | Frame interval p99 |
| ----------------- | ------: | --------------: | ----------------------: | --------------------------------------------: | -----------------: |
| RN Streamdown     |   2.00× |           0.44× |                   1.11× |                                         0.88× |              1.00× |
| Enriched Markdown |   1.75× |           1.10× |                   1.35× |                                         0.36× |              1.00× |

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

- **Live input.** One Release simulator app holds every library. Each run is a cold process showing one library full screen (five stacked panes for the concurrent workload, all visible). The Mac streams fresh model replies and forwards each delta over loopback as it arrives; the app appends it immediately and the harness checks that the app rendered exactly the text the model streamed. The window starts at the first delta, so model thinking time is not measured.
- **Feature coverage.** New captures and live runs request all eight subsets of math, code and tables, including every ordering, in one 16-section reply. Existing replies also mix nested formatting, lists, tasks, links, alerts, footnotes and Unicode. Huge replies rotate the six three-feature orderings. This covers content combinations, not every renderer option. Model output can omit requested constructs; inspect the recorded or live text before claiming complete coverage. Older captures retain their original content and must be recaptured for this matrix.
- **Math.** Math parsing is enabled for the matrix, concurrent and huge workloads. Downstream uses its installed math package; Streamdown and Enriched use their native math engines. Other libraries receive the same source with their default handling. Unsupported features can remain literal, so rendering and performance are not equivalent.
- **Frames.** A dylib injected at launch adds a CADisplayLink at the display’s maximum rate and run-loop observers to the main thread. A frame is a refresh interval in which the main thread worked for at least 0.2 ms; Fabric mounting, layout, text drawing and the Core Animation commit all run there. A frame is hitched when the main thread missed a refresh (the interval exceeds 1.5 refreshes). Frame interval is the time between the display-link callbacks around it, and frame delay is that interval minus one refresh. Hitch time ratio is Apple’s metric: time beyond one refresh summed over hitched intervals, per second of window. The simulator’s render server and GPU work outside the app and are not measured, and Xcode’s Animation Hitches instrument does not support the simulator.
- **CPU.** The host reads every app thread’s CPU time (`proc_pidinfo`) and the process total (`proc_pid_rusage`) every 25 ms and interpolates linearly at the window edges. App CPU is the process total, so it includes threads that start and end between samples. The breakdown is the main thread, the JS thread (com.facebook.react.runtime.JavaScript) and everything else (Hermes GC, native workers, worklets, exited threads). Compositing happens in the simulator’s render server, another process.
- **Memory.** Physical footprint, the figure Xcode and jetsam use, from the same 25 ms samples. "Peak increase" is the highest value during the window minus the value when streaming started.
- **Settle.** Time from the last delivered chunk to the last completed frame with main-thread work. New runs also split last text to app finish receipt from finish receipt to last frame. Finish receipt is separate from host done, which can wait for provider usage metadata. Raw runs contain per-thread boundaries and scheduler/frame attribution before and after finish. Missing boundaries in older runs remain unknown.
- **Animation profiles.** `highlighted` and `plain` disable streaming animations. `highlighted-animated` and `plain-animated` enable each supported library's default effect and include only Downstream, Streamdown and Enriched. These effects differ: Enriched also changes incomplete block presentation and defers code highlighting. Compare profiles with the same replay recording; live replies differ. The 150 ms renderer-tail target applies to animation-off runs. Open frames or app drawing at/after the window conservatively mark the tail as censored. Later frames can include benchmark activity; inspect the retained trace before attributing them to the renderer. Use `--settle-ms` to extend the same observation budget for all profiles.
- **JS lateness.** How late the app delivered each chunk against its schedule, a proxy for a busy JS thread.
- **Window.** From the moment streaming starts until 2 s after stream done is received (after the final chunk in replay runs). App start-up and mounting are outside it.
- **Order and noise.** 5 measured blocks plus one warmup block; every block runs each library and workload once in a seeded random order (seed 1). Before each run the app is terminated and the harness waits until the Mac reports nominal thermal pressure. The simulator is dedicated to the benchmark. Other Mac processes still share the CPU; the Mac ran on Battery Power.
- **Statistics.** Medians of per-run values, percentile bootstrap 95% intervals (4,000 resamples). Ratios are bootstrapped from both groups. Mann-Whitney U is two-sided with a normal approximation; no multiple-comparison correction, so read single stars with care.
- **Limits.** A simulator, not an iPhone: the Mac’s cores, clocks and GPU, no iOS thermal or power management, and the simulator’s render server rather than an iPhone display pipeline. Use it for ratios between libraries and before/after comparisons, and confirm absolute claims on a device. The common app contains every library, so memory is not a per-package size. Markdown output differs between libraries (see Features); equal input is not equal rendering.

Raw runs, traces and the full statistics are in `results/ios-20261018` (summary.json). Build: `highlighted 268f2cec6eb1, highlighted-animated 268f2cec6eb1`, revision `b1e3276fde` with local changes.
