# Round-off delta sweep, 2026-10-18 (kite 0.1.11, flat 32-per-round guard)
# complete Fermat tests at bpw = kite_fit_bpw + d with --fft L; residue compared with a run at ~L*1.125

## M4 Pro (wolf)
```
L=28672 k=2007 d=+0.00 n=432899 careful=8 (18/M sq) retries=0 end_L=28672 maxerr=0.375 29.1s OK
L=28672 k=2007 d=+0.05 n=434333 careful=33 (76/M sq) retries=1 end_L=30720 maxerr=0.125 31.5s OK
L=28672 k=2007 d=+0.10 n=435767 careful=2 (5/M sq) retries=1 end_L=30720 maxerr=0.4062 31.8s OK
L=28672 k=2007 d=+0.15 n=437201 careful=3 (7/M sq) retries=1 end_L=30720 maxerr=0.4375 32.2s OK
L=28672 k=2007 d=+0.20 n=438635 careful=4 (9/M sq) retries=1 end_L=30720 maxerr=0.4375 32.2s OK
L=28672 k=2007 d=+0.25 n=440067 careful=2 (5/M sq) retries=1 end_L=30720 maxerr=0.4062 32.2s OK
L=28672 k=2007 d=+0.30 n=441501 careful=2 (5/M sq) retries=1 end_L=30720 maxerr=0.4375 32.3s OK
L=49152 k=6767 d=+0.00 n=687363 careful=6 (9/M sq) retries=0 end_L=49152 maxerr=0.375 80.4s OK
L=49152 k=6767 d=+0.05 n=689821 careful=2 (3/M sq) retries=0 end_L=49152 maxerr=0.3906 80.7s OK
L=49152 k=6767 d=+0.10 n=692279 careful=63 (91/M sq) retries=0 end_L=49152 maxerr=0.375 81.0s OK
L=49152 k=6767 d=+0.15 n=694735 careful=33 (48/M sq) retries=1 end_L=51200 maxerr=0.2812 90.1s OK
L=49152 k=6767 d=+0.20 n=697193 careful=21 (30/M sq) retries=1 end_L=51200 maxerr=0.4062 92.4s OK
L=49152 k=6767 d=+0.25 n=699651 careful=11 (16/M sq) retries=1 end_L=51200 maxerr=0.4375 92.0s OK
L=49152 k=6767 d=+0.30 n=702109 careful=5 (7/M sq) retries=1 end_L=51200 maxerr=0.4375 92.5s OK
L=65536 k=2007 d=+0.00 n=965645 careful=0 (0/M sq) retries=0 end_L=65536 maxerr=0.375 151.8s OK
L=65536 k=2007 d=+0.05 n=968923 careful=1 (1/M sq) retries=0 end_L=65536 maxerr=0.375 152.9s OK
L=65536 k=2007 d=+0.10 n=972199 careful=7 (7/M sq) retries=0 end_L=65536 maxerr=0.375 153.3s OK
L=65536 k=2007 d=+0.15 n=975477 careful=19 (19/M sq) retries=0 end_L=65536 maxerr=0.375 153.6s OK
L=65536 k=2007 d=+0.20 n=978753 careful=33 (34/M sq) retries=1 end_L=69120 maxerr=0.2188 177.4s OK
L=65536 k=2007 d=+0.25 n=982029 careful=4 (4/M sq) retries=1 end_L=69120 maxerr=0.4375 183.6s OK
L=65536 k=2007 d=+0.30 n=985307 careful=15 (15/M sq) retries=1 end_L=69120 maxerr=0.4375 184.9s OK
L=65536 k=6767 d=+0.00 n=908189 careful=0 (0/M sq) retries=0 end_L=65536 maxerr=0.375 143.4s OK
L=65536 k=6767 d=+0.05 n=911465 careful=0 (0/M sq) retries=0 end_L=65536 maxerr=0.375 143.8s OK
L=65536 k=6767 d=+0.10 n=914741 careful=6 (7/M sq) retries=0 end_L=65536 maxerr=0.375 144.4s OK
L=65536 k=6767 d=+0.15 n=918019 careful=11 (12/M sq) retries=0 end_L=65536 maxerr=0.375 145.0s OK
L=65536 k=6767 d=+0.20 n=921295 careful=33 (36/M sq) retries=1 end_L=69120 maxerr=0.25 167.7s OK
L=65536 k=6767 d=+0.25 n=924573 careful=33 (36/M sq) retries=1 end_L=69120 maxerr=0.25 171.7s OK
L=65536 k=6767 d=+0.30 n=927849 careful=2 (2/M sq) retries=1 end_L=69120 maxerr=0.4375 174.3s OK
```

## M4 (eagle)
```
L=98304 k=6119 d=+0.00 n=1351881 careful=0 (0/M sq) retries=0 end_L=98304 maxerr=0.375 337.5s OK
L=98304 k=6119 d=+0.05 n=1356797 careful=2 (1/M sq) retries=0 end_L=98304 maxerr=0.375 338.7s OK
L=98304 k=6119 d=+0.10 n=1361711 careful=16 (12/M sq) retries=0 end_L=98304 maxerr=0.3906 340.2s OK
L=98304 k=6119 d=+0.15 n=1366627 careful=33 (24/M sq) retries=1 end_L=102400 maxerr=0.25 360.1s OK
L=98304 k=6119 d=+0.20 n=1371541 careful=22 (16/M sq) retries=1 end_L=102400 maxerr=0.4062 369.9s OK
L=98304 k=6119 d=+0.25 n=1376457 careful=24 (17/M sq) retries=1 end_L=102400 maxerr=0.4062 369.1s OK
L=98304 k=6119 d=+0.30 n=1381373 careful=3 (2/M sq) retries=1 end_L=102400 maxerr=0.4062 368.1s OK
L=131072 k=4813 d=+0.00 n=1808615 careful=0 (0/M sq) retries=0 end_L=131072 maxerr=0.375 606.1s OK
L=131072 k=4813 d=+0.05 n=1815169 careful=0 (0/M sq) retries=0 end_L=131072 maxerr=0.375 609.1s OK
L=131072 k=4813 d=+0.10 n=1821723 careful=8 (4/M sq) retries=0 end_L=131072 maxerr=0.375 610.8s OK
L=131072 k=4813 d=+0.15 n=1828277 careful=15 (8/M sq) retries=0 end_L=131072 maxerr=0.375 613.1s OK
L=131072 k=4813 d=+0.20 n=1834829 careful=18 (10/M sq) retries=1 end_L=138240 maxerr=0.4375 728.9s OK
L=131072 k=4813 d=+0.25 n=1841383 careful=33 (18/M sq) retries=1 end_L=138240 maxerr=0.25 701.8s OK
L=131072 k=4813 d=+0.30 n=1847937 careful=9 (5/M sq) retries=1 end_L=138240 maxerr=0.4062 750.9s OK
```

4813*2^5500155-1 at 409600 (+0.13 past the fit), cost-based guard: RES64 D6321DD51FA5FB38 (matches), 6487.7 s, 1179.5 us/iter, 349 careful ops, 0 FFT retries, max error 0.3906 (M4 Pro, one core).
