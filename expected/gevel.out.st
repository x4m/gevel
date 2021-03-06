SET client_min_messages = warning;
\set ECHO none
RESET client_min_messages;
CREATE TABLE gevelt ( t box );
\copy gevelt from 'data/rect.data'
SELECT center(t) AS p INTO gevelp FROM gevelt;
CREATE INDEX gist_idx ON gevelt USING gist ( t );
CREATE INDEX spgist_idx ON gevelp USING spgist ( p );
ERROR:  access method "spgist" does not exist
CREATE INDEX kdspgist_idx ON gevelp USING spgist ( p kd_point_ops);
ERROR:  access method "spgist" does not exist
--GiST
SELECT gist_stat('gist_idx');
                gist_stat                
-----------------------------------------
 Number of levels:          2
 Number of pages:           33
 Number of leaf pages:      32
 Number of tuples:          3410
 Number of invalid tuples:  0
 Number of leaf tuples:     3378
 Total size of tuples:      143740 bytes
 Total size of leaf tuples: 142344 bytes
 Total size of index:       270336 bytes
 
(1 row)

SELECT gist_tree('gist_idx');
                                           gist_tree                                           
-----------------------------------------------------------------------------------------------
 0(l:0) blk: 0 numTuple: 32 free: 6764b(17.11%) rightlink:4294967295 (InvalidBlockNumber)
     1(l:1) blk: 2 numTuple: 278 free: 2588b(68.28%) rightlink:4294967295 (InvalidBlockNumber)
     2(l:1) blk: 18 numTuple: 109 free: 3352b(58.92%) rightlink:3 (OK)
     3(l:1) blk: 3 numTuple: 98 free: 3836b(52.99%) rightlink:20 (OK)
     4(l:1) blk: 20 numTuple: 114 free: 3132b(61.62%) rightlink:16 (OK)
     5(l:1) blk: 13 numTuple: 110 free: 3308b(59.46%) rightlink:18 (OK)
     6(l:1) blk: 6 numTuple: 102 free: 3660b(55.15%) rightlink:22 (OK)
     7(l:1) blk: 22 numTuple: 102 free: 3660b(55.15%) rightlink:13 (OK)
     8(l:1) blk: 8 numTuple: 103 free: 3616b(55.69%) rightlink:19 (OK)
     9(l:1) blk: 9 numTuple: 97 free: 3880b(52.45%) rightlink:25 (OK)
     10(l:1) blk: 26 numTuple: 100 free: 3748b(54.07%) rightlink:7 (OK)
     11(l:1) blk: 10 numTuple: 94 free: 4012b(50.83%) rightlink:26 (OK)
     12(l:1) blk: 24 numTuple: 96 free: 3924b(51.91%) rightlink:10 (OK)
     13(l:1) blk: 4 numTuple: 88 free: 4276b(47.60%) rightlink:27 (OK)
     14(l:1) blk: 27 numTuple: 93 free: 4056b(50.29%) rightlink:12 (OK)
     15(l:1) blk: 17 numTuple: 91 free: 4144b(49.22%) rightlink:28 (OK)
     16(l:1) blk: 28 numTuple: 99 free: 3792b(53.53%) rightlink:4 (OK)
     17(l:1) blk: 30 numTuple: 91 free: 4144b(49.22%) rightlink:2 (OK)
     18(l:1) blk: 5 numTuple: 104 free: 3572b(56.23%) rightlink:24 (OK)
     19(l:1) blk: 15 numTuple: 92 free: 4100b(49.75%) rightlink:29 (OK)
     20(l:1) blk: 7 numTuple: 90 free: 4188b(48.68%) rightlink:21 (OK)
     21(l:1) blk: 29 numTuple: 100 free: 3748b(54.07%) rightlink:9 (OK)
     22(l:1) blk: 25 numTuple: 96 free: 3924b(51.91%) rightlink:17 (OK)
     23(l:1) blk: 1 numTuple: 99 free: 3792b(53.53%) rightlink:23 (OK)
     24(l:1) blk: 21 numTuple: 107 free: 3440b(57.84%) rightlink:11 (OK)
     25(l:1) blk: 14 numTuple: 90 free: 4188b(48.68%) rightlink:31 (OK)
     26(l:1) blk: 31 numTuple: 90 free: 4188b(48.68%) rightlink:5 (OK)
     27(l:1) blk: 32 numTuple: 88 free: 4276b(47.60%) rightlink:8 (OK)
     28(l:1) blk: 12 numTuple: 164 free: 932b(88.58%) rightlink:6 (OK)
     29(l:1) blk: 19 numTuple: 94 free: 4012b(50.83%) rightlink:14 (OK)
     30(l:1) blk: 11 numTuple: 94 free: 4012b(50.83%) rightlink:30 (OK)
     31(l:1) blk: 23 numTuple: 101 free: 3704b(54.61%) rightlink:15 (OK)
     32(l:1) blk: 16 numTuple: 104 free: 3572b(56.23%) rightlink:32 (OK)
 
(1 row)

SELECT * FROM gist_print('gist_idx') as t(level int, valid bool, a box) where level=1;
 level | valid |              a              
-------+-------+-----------------------------
     1 | t     | 
     1 | t     | (24240,50012),(204,46917)
     1 | t     | (28102,24428),(25047,579)
     1 | t     | (31193,24679),(28142,6)
     1 | t     | (24938,46902),(64,43729)
     1 | t     | (24898,40568),(3,37480)
     1 | t     | (24647,43650),(179,40585)
     1 | t     | (32737,49923),(31178,1038)
     1 | t     | (25002,6338),(12350,51)
     1 | t     | (49985,25005),(46856,499)
     1 | t     | (46830,24767),(43743,222)
     1 | t     | (43689,24613),(40603,43)
     1 | t     | (6348,37358),(120,25034)
     1 | t     | (12528,37333),(6305,24861)
     1 | t     | (25054,18890),(12555,12796)
     1 | t     | (24855,24987),(12403,18887)
     1 | t     | (50027,49912),(37338,43927)
     1 | t     | (40565,24963),(37375,491)
     1 | t     | (9308,24151),(6200,620)
     1 | t     | (43715,37586),(37415,25070)
     1 | t     | (12343,24542),(9295,294)
     1 | t     | (24919,12698),(12654,6518)
     1 | t     | (3088,24465),(15,81)
     1 | t     | (49999,37405),(43837,25153)
     1 | t     | (35828,49443),(34242,972)
     1 | t     | (37357,50073),(35769,357)
     1 | t     | (31171,50040),(24900,37723)
     1 | t     | (24840,37414),(12552,24965)
     1 | t     | (34270,49382),(32763,594)
     1 | t     | (49957,43846),(37423,37741)
     1 | t     | (6197,24629),(3113,108)
     1 | t     | (31179,37276),(24921,25000)
(32 rows)

--SPGiST
SELECT spgist_stat('spgist_idx');
NOTICE:  Function is not working under PgSQL < 9.2
 spgist_stat 
-------------
 ???
(1 row)

SELECT * FROM spgist_print('kdspgist_idx') as t(tid tid, node_n int, level int, tid_pointer tid, prefix float8, node_label int, leaf_value point);
NOTICE:  Function is not working under PgSQL < 9.2
ERROR:  cache lookup failed for type 2779096485
SELECT * FROM spgist_print('spgist_idx') as t(tid tid, node_n int, level int, tid_pointer tid, prefix point, node_label int, leaf_value point) WHERE level = 1;
NOTICE:  Function is not working under PgSQL < 9.2
ERROR:  cache lookup failed for type 2779096485
--GIN 
CREATE TABLE test__int( a int[] );
\copy test__int from 'data/test__int.data'
CREATE INDEX gin_idx ON test__int USING gin ( a );
INSERT INTO test__int ( SELECT ARRAY[t] || '{1000}'::_int4 FROM generate_series (1,300) as t );
INSERT INTO test__int ( SELECT ARRAY[t] || '{1001}'::_int4 FROM generate_series (1,300) as t, generate_series(1,12) );
VACUUM ANALYZE test__int; 
SELECT * FROM gin_stat('gin_idx') as t(value int, nrow int);
 value | nrow 
-------+------
     0 |   38
     1 |  257
     2 |  244
     3 |  222
     4 |  228
     5 |  227
     6 |  243
     7 |  238
     8 |  236
     9 |  224
    10 |  236
    11 |  248
    12 |  224
    13 |  235
    14 |  237
    15 |  253
    16 |  234
    17 |  245
    18 |  246
    19 |  234
    20 |  236
    21 |  213
    22 |  235
    23 |  222
    24 |  197
    25 |  253
    26 |  218
    27 |  239
    28 |  198
    29 |  244
    30 |  247
    31 |  267
    32 |  234
    33 |  209
    34 |  231
    35 |  218
    36 |  242
    37 |  232
    38 |  221
    39 |  232
    40 |  241
    41 |  239
    42 |  221
    43 |  211
    44 |  231
    45 |  213
    46 |  236
    47 |  264
    48 |  221
    49 |  235
    50 |  219
    51 |  250
    52 |  270
    53 |  222
    54 |  242
    55 |  237
    56 |  237
    57 |  251
    58 |  231
    59 |  272
    60 |  219
    61 |  233
    62 |  235
    63 |  260
    64 |  252
    65 |  221
    66 |  228
    67 |  253
    68 |  196
    69 |  232
    70 |  232
    71 |  223
    72 |  247
    73 |  214
    74 |  232
    75 |  229
    76 |  233
    77 |  221
    78 |  227
    79 |  233
    80 |  216
    81 |  244
    82 |  223
    83 |  254
    84 |  227
    85 |  247
    86 |  255
    87 |  239
    88 |  258
    89 |  249
    90 |  244
    91 |  226
    92 |  225
    93 |  230
    94 |  218
    95 |  232
    96 |  239
    97 |  226
    98 |  209
    99 |  211
   100 |  216
   101 |   49
   102 |   47
   103 |   59
   104 |   55
   105 |   48
   106 |   49
   107 |   49
   108 |   51
   109 |   47
   110 |   51
   111 |   45
   112 |   46
   113 |   48
   114 |   38
   115 |   39
   116 |   43
   117 |   44
   118 |   46
   119 |   45
   120 |   52
   121 |   41
   122 |   64
   123 |   50
   124 |   41
   125 |   55
   126 |   41
   127 |   50
   128 |   54
   129 |   43
   130 |   44
   131 |   50
   132 |   57
   133 |   40
   134 |   41
   135 |   44
   136 |   58
   137 |   48
   138 |   44
   139 |   50
   140 |   49
   141 |   48
   142 |   43
   143 |   36
   144 |   43
   145 |   49
   146 |   46
   147 |   40
   148 |   52
   149 |   46
   150 |   49
   151 |   42
   152 |   58
   153 |   49
   154 |   51
   155 |   49
   156 |   45
   157 |   53
   158 |   59
   159 |   38
   160 |   53
   161 |   48
   162 |   41
   163 |   40
   164 |   52
   165 |   50
   166 |   40
   167 |   48
   168 |   45
   169 |   49
   170 |   50
   171 |   45
   172 |   46
   173 |   47
   174 |   41
   175 |   51
   176 |   50
   177 |   41
   178 |   42
   179 |   48
   180 |   47
   181 |   49
   182 |   40
   183 |   46
   184 |   52
   185 |   36
   186 |   45
   187 |   41
   188 |   47
   189 |   49
   190 |   46
   191 |   45
   192 |   39
   193 |   50
   194 |   39
   195 |   54
   196 |   43
   197 |   50
   198 |   46
   199 |   42
   200 |   31
   201 |   28
   202 |   28
   203 |   28
   204 |   30
   205 |   22
   206 |   25
   207 |   27
   208 |   32
   209 |   22
   210 |   21
   211 |   31
   212 |   22
   213 |   37
   214 |   19
   215 |   25
   216 |   27
   217 |   26
   218 |   24
   219 |   25
   220 |   26
   221 |   21
   222 |   23
   223 |   20
   224 |   26
   225 |   25
   226 |   25
   227 |   23
   228 |   30
   229 |   30
   230 |   24
   231 |   22
   232 |   27
   233 |   27
   234 |   21
   235 |   27
   236 |   24
   237 |   24
   238 |   29
   239 |   32
   240 |   31
   241 |   24
   242 |   36
   243 |   21
   244 |   29
   245 |   22
   246 |   23
   247 |   21
   248 |   26
   249 |   29
   250 |   24
   251 |   29
   252 |   25
   253 |   28
   254 |   25
   255 |   19
   256 |   23
   257 |   20
   258 |   24
   259 |   31
   260 |   29
   261 |   20
   262 |   25
   263 |   23
   264 |   25
   265 |   23
   266 |   31
   267 |   22
   268 |   26
   269 |   33
   270 |   25
   271 |   27
   272 |   29
   273 |   29
   274 |   26
   275 |   28
   276 |   30
   277 |   28
   278 |   29
   279 |   22
   280 |   29
   281 |   23
   282 |   30
   283 |   29
   284 |   23
   285 |   37
   286 |   27
   287 |   31
   288 |   28
   289 |   26
   290 |   25
   291 |   29
   292 |   22
   293 |   26
   294 |   29
   295 |   24
   296 |   27
   297 |   30
   298 |   29
   299 |   26
   300 |   13
  1000 |  300
  1001 | 3834
(303 rows)

