Apple M4 Pro, 10 P + 4 E, kite version 0.1.10, PRST version 14.0.1888, GWnum library version 31.6, GMP library version 6.3.0, 45 s per run

| Case | Tests | kite FFT | kite ms/sq | kite tests/h | PRST FFT | PRST ms/sq | PRST tests/h | kite vs PRST |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| 1021·2^2300003−1 | 14 | 160K | 0.685 | 32.0 | 160K | 0.772 | 28.4 | 1.13× |
| 6073·2^2290815−1 | 14 | 175K | 0.837 | 26.3 | 192K | 0.917 | 24.0 | 1.10× |
| 2009·2^3322280−1 | 14 | 240K | 1.216 | 12.5 | 240K | 1.224 | 12.4 | 1.01× |
| 6767·2^3322174−1 | 14 | 256K | 1.368 | 11.1 | 256K | 1.351 | 11.2 | 0.99× |
| 999·2^4000000−1 | 14 | 288K | 1.658 | 7.6 | 288K | 1.713 | 7.4 | 1.03× |
| 6119·2^4838048−1 | 14 | 384K | 2.444 | 4.3 | 384K | 2.450 | 4.3 | 1.00× |
| 4813·2^5500155−1 | 14 | 432K | 3.095 | 3.0 | 400K | 2.905 | 3.2 | 0.94× |

384K and 432K rerun at 60 s per run: at 45 s PRST printed no progress after its first line.
KITE_FFT_TIMES=/nonexistent, so kite used its built-in table, as a client machine does.
