# KrgnMiner — mineur GPU NVIDIA pour Kerrigan (KRGN), Equihash 192,7

KrgnMiner mine [Kerrigan](https://kerrigan.network/) (KRGN) sur GPU NVIDIA avec l'algo **Equihash(192,7)** et la
personnalisation BLAKE2b `kerrigan`, sur le stratum de [rplant](https://pool.rplant.xyz/) (style Zcash).
Le solveur CUDA (algorithme de Wagner) a été écrit de zéro pour les cartes Pascal (P104-100, GTX 1070/1080, sm_61).

- **Aucun frais de développement.**
- Open source, Apache-2.0.
- Version Windows x64 compilée avec CUDA 12.9 (code sm_61 + PTX : les GPU plus récents l'exécutent via la
  compilation JIT du pilote).
- Chaque solution trouvée par le GPU est revérifiée par le CPU (collisions, ordre, indices distincts) avant d'être
  envoyée au pool.

English version: [README.md](README.md).
Le même code de solveur sert à [YecMiner](https://github.com/gacanga/YecMiner) (Ycash, Equihash 192,7) et à
[NullMiner](https://github.com/gacanga/NullMiner) (NULL network, Equihash 144,5).

## Démarrage rapide (Windows)

1. Téléchargez le zip sur la page [Releases](https://github.com/gacanga/KrgnMiner/releases) et décompressez-le.
2. Ouvrez `krgnminer.conf` et remplacez `YOUR_KRGN_ADDRESS` par votre adresse Kerrigan (elle commence par `K`) ;
   `rig1` est le nom du worker, modifiable.
3. Double-cliquez sur `start_krgn.bat`. Il relance le mineur 10 s après tout arrêt et écrit le journal dans
   `krgnminer.log` ; fermez la fenêtre pour arrêter.

`test.bat` lance les tests de calcul sur chaque GPU, `bench.bat` une mesure de 60 s en Sol/s (arrêtez le minage
avant).

Prérequis : Windows 10/11 x64, un pilote NVIDIA compatible CUDA 12 (R525 ou plus récent ; R580 est la dernière
branche pour Pascal), environ **5,4 Go de mémoire GPU libre par carte** (cartes 8 Go). Rien d'autre à installer.

## Pool

rplant, port Equihash 192,7 de Kerrigan : `stratum+tcp://stratum-eu.rplant.xyz:7040`
(TLS : `stratum+ssl://stratum-eu.rplant.xyz:17040`, secours `stratum-ru.rplant.xyz`), utilisateur
`ADRESSE_KRGN.rig`, mot de passe `x`, 1 % de frais, PROP, paiement minimum 1 KRGN.

Protocole (Equihash NOMP) :

```
-> mining.subscribe [agent]                  <- [session, extranonce1]
-> mining.authorize [user, pass]             <- true
<- mining.set_target [cible_hex_be]          (ou mining.set_difficulty [d], diff 1 = 0x0007ffff << 224)
<- mining.notify [job, version, prevhash, merkleroot, reserved, time, bits, clean, "192_7", "kerrigan"]
-> mining.submit [user, job, time, nonce2, "fd9001" + solution (400 octets)]
```

Le nonce de 32 octets = extranonce1 suivi de nonce2. Une share est valide quand
SHA256d(en-tête ‖ fd9001 ‖ solution) est sous la cible des shares.

## Configuration

Le mineur lit `krgnminer.conf` à côté de l'exécutable (ou un dossier au-dessus), ou le fichier donné par
`--config`. Une ligne = `clé = valeur` ; `#` et `;` commencent un commentaire. Chaque clé est aussi une option de
ligne de commande (`--clé valeur`) ; la ligne de commande l'emporte sur le fichier, et `-o` en ligne de commande
remplace les pools du fichier.

| Option / clé | Défaut | Rôle |
| --- | --- | --- |
| `-o`, `--pool`, `--url` / `pool` | — | URL du pool, répétable (ordre de secours). `stratum+tcp://` = TCP simple, `stratum+ssl://` ou `stratum+tls://` = TLS |
| `-u`, `--user` / `user` | — | `ADRESSE_KRGN.rig` |
| `-p`, `--pass` / `pass` | `x` | Mot de passe du pool |
| `--gpus`, `--devices` / `gpus` | `all` | `all` ou une liste comme `0,1,2` (ordre du bus PCI, comme nvidia-smi) |
| `--tpb` / `tpb` | `256` | Threads par bloc des kernels de collision, multiple de 32 (32 à 1024) |
| `--stats` / `stats` | `30` | Secondes entre deux lignes de résumé (minimum 5) |
| `--log` / `log` | — | Ajoute le journal à ce fichier |
| `--cafile` / `cafile` | `ca-bundle.crt` à côté de l'exe | Certificats racines pour les pools TLS |
| `--config`, `-c` | `krgnminer.conf` | Fichier de configuration |
| `--args-file FICHIER` | — | Lit d'autres arguments dans un fichier (séparés par des espaces) |
| `--test` | | Tests de calcul (CPU, puis chaque GPU choisi), code de sortie 0 si tout passe |
| `--bench [s]` | 60 s | Mesure des Sol/s sur les GPU choisis |
| `--profile` | | Temps de chaque étape du solveur (moyenne de 3 passes) |
| `--help`, `-h` | | Aide |

Codes de sortie utilisés par la boucle de relance : 2 = argument invalide ou pool/utilisateur manquant,
3 = erreur CUDA, 4 = une passe GPU n'est pas revenue en 20 s (fréquences instables ?), 5 = délai dépassé en
test/bench.

## Performances (mesurées)

NVIDIA P104-100 8 Go, réglages d'usine, pool rplant :

| Configuration | Sol/s |
| --- | --- |
| 3 × P104-100 | 42–43 Sol/s (≈ 14 Sol/s par carte) |

Sur une session de 4 heures au pool : 103 shares acceptées, aucune rejetée.
Le solveur est limité par la bande passante mémoire (accès dispersés de 32 octets).
Avec le même solveur, YecMiner a mesuré 68 Sol/s sur 5 × P104-100 aux réglages d'usine (voir le README de YecMiner).

## Fonctionnement

Algorithme de Wagner : 2^24 BLAKE2b-384 → 2^25 chaînes de 24 octets (deux par hash) :

- 6 étapes de collision sur des chiffres de 24 bits, une étape finale sur les 48 derniers bits, puis la
  reconstruction des 128 indices de chaque candidat ;
- chaque niveau est rangé en 4 096 seaux × 8 704 emplacements ; dans un seau, les bits restants sont appariés par
  des listes chaînées en mémoire partagée, phases de chaînage et d'appariement fusionnées ;
- chaque chaîne est un seul enregistrement qui contient sa donnée et son entrée d'arbre (32, 24 ou 16 octets selon
  le niveau), car sur ce GPU une écriture dispersée coûte un secteur de 32 octets, quel que soit le nombre d'octets
  écrits ;
- ≈ 5,4 Go de mémoire GPU par carte.

L'hôte calcule l'état intermédiaire (midstate) BLAKE2b de l'en-tête de 140 octets ; le GPU termine les hashs.
Les mêmes fonctions de phase (`gpu/eq_common.h`) sont compilées pour CUDA (`gpu/eq_gpu.cu`) et en émulation CPU
séquentielle, qui sert à générer les vecteurs de test. Les options de compilation de la disposition mémoire sont
décrites en tête de `gpu/eq_common.h`.

## Compilation (Windows)

Prérequis attendus par les scripts :

- CUDA Toolkit **12.9** (`CUDA_PATH_V12_9`, par défaut `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9`) ;
  CUDA 13 ne cible plus Pascal.
- Visual Studio 2022 (ou Build Tools) avec les outils C++ x64 (trouvés par `vswhere`).
- MSYS2 dans `C:\msys64` avec les paquets MINGW64 `mingw-w64-x86_64-gcc` et `mingw-w64-x86_64-openssl`.

| Script | Rôle |
| --- | --- |
| `build_all.bat` | Compile `dev\kgpu.dll` (nvcc + MSVC, `windows\build_gpu_dll.bat`) et `dev\krgnminer.exe` (MSYS2 g++, `windows/build_host.sh`), puis lance les tests. `build_all.bat nobuild` = tests seuls |
| `run_test.bat` | `dev\krgnminer.exe --test`, journal `logs\test.log` |
| `run_bench.bat` | Mesure de 60 s de `dev\`, journal `logs\bench.log` ; arguments en plus dans `run\bench_args.txt` ; avec `run\variants.txt`, compile et profile des variantes de disposition (`windows\build_variants.bat`) |
| `deploy.bat` | Copie `dev\` vers `bin\`, seulement si le dernier `logs\test.log` indique `TEST RESULT: ALL OK` |
| `krgn_pool.bat` | Mine avec `bin\krgnminer.exe` et `krgnminer.conf`, relance automatique, journal `logs\pool.log` ; arguments en plus dans `run\pool_args.txt` |
| `stop_krgn.bat` | Arrête la boucle de relance et `krgnminer.exe` |

Options de compilation (variables d'environnement) : `KM_CRT=/MT` lie le runtime C en statique dans `kgpu.dll`,
`KM_NVCC_EXTRA` (ou `run\nvcc_extra.txt`) ajoute des options nvcc, `KM_HOST_ARCH` remplace `-march=native`. Pour
une version portable : `set KM_CRT=/MT` et `set KM_HOST_ARCH=-march=x86-64 -mtune=generic` avant `build_all.bat`.
Le zip publié contient `krgnminer.exe`, `kgpu.dll`, `cudart64_12.dll`, `ca-bundle.crt` et les fichiers de
[`dist/`](dist).

## Tests

`krgnminer.exe --test` :

1. CPU : réponses connues du BLAKE2b-384 `kerrigan` (calculées avec hashlib de Python), sur les deux chemins de
   calcul du hash ;
2. CPU : les solutions de référence de `src/eq_vectors.h`, produites par le solveur officiel de Kerrigan
   (Kerrigan Core, `src/crypto/equihash.cpp`), doivent être valides et se réencoder à l'identique ; des variantes
   corrompues ou mal ordonnées doivent être rejetées ;
3. chaque GPU choisi : les 5 en-têtes de référence (d'un vrai job rplant, nonces variés) doivent donner exactement
   les solutions de référence et exactement le nombre de chaînes de chaque niveau de l'émulation CPU, puis
   10 nonces aléatoires, chaque solution étant revérifiée par le CPU.

`tests/eq_cloud_test.cpp` (Linux, libsodium) vérifie le code hôte et l'émulation CPU par rapport à libsodium et aux
solutions de référence ; `tools/gen_vectors.py` régénère `src/eq_vectors.h` à partir de sa sortie (les solutions
de référence viennent d'une compilation du solveur de Kerrigan, non fournie). Exemple :

```sh
g++ -O2 -std=gnu++17 -Isrc -Igpu -o eq_cloud_test tests/eq_cloud_test.cpp src/eq_host.cpp -lsodium
./eq_cloud_test <entete140hex> <solution de reference hex>...
```

`tools/stratum_probe.ps1` se connecte à un port stratum et journalise les lignes reçues (pas de minage, aucune
share).

## Crédits et licence

- Solveur, code hôte et client stratum : gacanga. Le client stratum (`src/json.h`, `src/tls_conn.*`) vient de
  FiloMiner (Apache-2.0).
- Kerrigan Core (MIT) a servi de référence de calcul pour les tests uniquement ; il n'est pas lié au mineur.

Licence Apache 2.0, voir [LICENSE](LICENSE) et [NOTICE](NOTICE). Aucune garantie : vous minez à vos risques, et le
minage consomme de l'électricité.
