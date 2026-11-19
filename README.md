# morphingAligner

2つの音声（base / target）のスペクトル包絡を並べて表示し、時間軸・周波数軸の対応（アンカー）を
打って、その対応にもとづく音声モーフィングを行う GUI アプリです。

- スペクトル包絡の表示（WORLD）、時間・周波数アンカーの編集
- アンカーにもとづくモーフィングと再生・WAV 保存（tcmorph）
- フォルマントの表示と、音素アライメント（Montreal Forced Aligner）にもとづくアンカーの自動生成
  （この2つには追加のセットアップが必要です。下の「音声解析のセットアップ」を参照してください）

## 動作環境

- Windows 10 / 11（x64）、Linux（x64。Ubuntu で確認）
- OpenGL 3.3 以上を推奨（3.0 でも動きますが、表示が多いと一部が欠けることがあります）
- 音声解析は日本語のみ対応です

## インストール

[Releases](https://github.com/shoma-knot/morphingAligner-Cpp/releases) から自分の OS 用の配布物を
ダウンロードして展開し、中の `morphingAlignerCpp`（Windows は `morphingAlignerCpp.exe`）を起動します。
展開したフォルダの構成は変えないでください（`font/` や `licenses/` を相対パスで読みます）。

Linux でタスクバーにアイコンを出したい場合は `tools/install-desktop-entry.sh` を実行します
（解除は `tools/uninstall-desktop-entry.sh`）。

## 音声解析のセットアップ（フォルマント・音素アライメント）

フォルマント推定（parselmouth）と音素アライメント（Montreal Forced Aligner）は Python で動くため、
配布物のフォルダの中に専用の Python 環境（`.env`）を作る必要があります。これが無くても、
スペクトル包絡の表示・アンカーの編集・モーフィングは使えます。

**必要なもの**: 約 5 GB の空き容量、インターネット接続、conda 系のツール（下記のどれか1つ）。

1. **conda 系のツールを入れる**（既に micromamba / mamba / conda を使っていれば不要）

   おすすめは micromamba です（単体の実行ファイルで、管理者権限は不要）。

   - Windows（PowerShell）
     ```powershell
     Invoke-Expression ((Invoke-WebRequest -Uri https://micro.mamba.pm/install.ps1 -UseBasicParsing).Content)
     ```
   - Linux
     ```sh
     "${SHELL}" <(curl -L micro.mamba.pm)
     ```

   詳しくは [micromamba のインストール手順](https://mamba.readthedocs.io/en/latest/installation/micromamba-installation.html)
   を参照してください。[Miniforge](https://conda-forge.org/download/) の conda / mamba でも構いません。
   入れたあとは、新しいターミナルを開いてから次へ進んでください。

2. **セットアップを実行する**（配布物のフォルダで）

   - Windows: `install-win.bat` をダブルクリック
   - Linux: `./install.sh`

   Python 環境の作成（初回は 10 分以上かかることがあります）、MFA の日本語モデルの取得、
   動作確認までを行います。最後に「音声解析の環境: 使えます」と出れば完了です。

3. **アプリで確認する**

   起動中なら、左パネルの「音声解析（Python）」の「再確認」を押します（起動し直しても構いません）。
   「未セットアップ」の表示が消え、音声を読み込むとフォルマントが推定されるようになります。

### うまくいかないとき

- アプリの「未セットアップ」にカーソルを合わせると、使えない理由が表示されます。
  下部のログにも理由が出ます。
- 同じスクリプトをもう一度実行すれば、セットアップをやり直せます。環境を作り直したいときは
  `.env` フォルダを削除してから実行してください。
- フォルダのパスや Windows のユーザー名に日本語などの ASCII 以外の文字が含まれていると、
  音素アライメントが失敗することがあります（ログに警告が出ます）。その場合は `C:\tools\` などの
  ASCII だけのパスに配布物を置いてください。
- MFA のモデルはユーザーのフォルダ（`Documents/MFA`）に保存されます。
- 使わなくなったら、`.env` フォルダと `Documents/MFA` フォルダを削除すれば元に戻ります。

## 基本的な使い方

1. 左パネルの Base / Target の「読み込む」で音声を開きます（wav / flac / mp3 / ogg）。
2. スペクトログラムを左クリックすると時間アンカーが追加されます。上下の線をドラッグして対応を合わせます。
   Ctrl を押しながらだと周波数アンカーの操作になります（操作の一覧は「アンカー: n」の横の (?) にあります）。
3. 音声解析をセットアップ済みなら、書き起こし（例: 「はい」）を入力して「音素アライメント」を押し、
   「アンカー自動生成」で音素境界とフォルマントにもとづくアンカーを打てます。
4. 「モーフィング」タブで率を動かすと、モーフィングした音声を合成・再生できます。
5. 「連続書き出し...」で、率を 0（Base）から 1（Target）まで等間隔に変えた WAV を、指定した本数だけまとめて
   書き出せます。実験用の刺激の連続体を作るときに使います。変化させる軸を選ぶと、
   選んだ軸だけを連動させ、ほかの軸は指定した率に固定します。
6. 作業内容は「セッション保存」で JSON に保存できます。

## ライセンス

- 配布物に含まれるライブラリ・フォントの条文は `licenses/` と `font/` にあり、アプリの
  「ライセンス表示」タブでも読めます。
- 音声解析のセットアップで入るソフトウェアは配布物には含まれず、各自の環境に入ります。
  主なものの条件は次のとおりです。
  - [Montreal Forced Aligner](https://github.com/MontrealCorpusTools/Montreal-Forced-Aligner): MIT
  - [Parselmouth](https://github.com/YannickJadoul/Parselmouth)（Praat）: GPL-3.0
  - [SudachiPy](https://github.com/WorksApplications/SudachiPy) / SudachiDict: Apache-2.0
  - MFA の日本語モデル（japanese_mfa）: [MFA のドキュメント](https://mfa-models.readthedocs.io/)の各モデルのページを参照してください
- Python 環境のパッケージは conda-forge から取得します（Anaconda の defaults チャンネルは使いません）。

## 開発者向け

ビルド手順や実装の詳細は [DEVLOG.md](DEVLOG.md) を参照してください。

### コマンドライン引数

最初にサブコマンドを書きます。`gui` は起動と同時に音声やセッションを読み込んで GUI を開きます（毎回ダイアログで
開く手間を省くためのものです）。`auto-anchors`・`convert`・`export` はウィンドウを開かずに動き、セッションの JSON を
受け渡します。引数なしで起動すると、何も読み込まずに GUI を開きます。引数があるのにサブコマンドが無いときは、
エラーを出して止まります。

| サブコマンド | 入力 | 出力 |
|---|---|---|
| `gui` | 音声・セッション（任意） | GUI を開く |
| `auto-anchors` | 音声2本（またはセッション）と書き起こし | アンカーを自動で作ったセッション |
| `convert` | tcmorph のアンカー JSON | セッション |
| `export` | セッション | 率を変えた WAV の列と `<接頭辞>_batch.json` |

```sh
morphingAlignerCpp gui --base a.wav --target b.wav                    # 音声を読み込む
morphingAlignerCpp gui --session work.json                             # セッションを読み込む
morphingAlignerCpp gui --base a.wav --target b.wav --session anchors.json   # tcmorph のアンカーを乗せる
morphingAlignerCpp gui --base a.wav --target b.wav --transcript はい --align  # 読み込み後に音素アライメント

morphingAlignerCpp auto-anchors --base a.wav --target b.wav --transcript はい --out auto.json   # アンカーを自動で作る
morphingAlignerCpp convert for-cpp/anchors.json --out work.json        # tcmorph の anchors.json と manifest.json をセッションにする
morphingAlignerCpp export work.json --count 7 --out stim/              # 全軸を連動させて 7 本
morphingAlignerCpp export work.json --count 11 --out stim/ --sweep fo --fixed tx=0.5   # F0 だけを変える
```

`gui` のオプションは次のとおりです。

| オプション | 内容 |
|---|---|
| `--base <音声>` / `--target <音声>` | base / target の音声を読み込む |
| `--session <JSON>` | セッションを読み込む。tcmorph のアンカー JSON なら、`--base` / `--target` の音声にアンカーだけを乗せる |
| `--transcript <文>` | 音素アライメント用の書き起こし（セッションの書き起こしより優先） |
| `--align` | 読み込み後、音声解析の環境が使えたら音素アライメントを実行する（`--transcript` か、書き起こしを含むセッションが必要） |

`auto-anchors` のオプションは次のとおりです。GUI の「アンカー自動生成」と同じ方法でアンカーを作ります。
音声解析のセットアップが必要です。

| オプション | 内容 |
|---|---|
| `--base <音声>` / `--target <音声>` | base / target の音声（両方を指定する） |
| `--session <JSON>` | 音声のパス（と書き起こし）を含むセッション。中のアンカーは使わずに作り直す |
| `--transcript <文>` | 書き起こし（セッションの書き起こしより優先） |
| `--out <JSON>` | 保存するセッション（必須） |
| `--divisions <n>` | 各音素の区間を何等分するか（1〜10。既定: 2） |
| `--formant-window <ms>` | フォルマントの移動平均の窓幅（5〜500。既定: 50） |
| `--max-formant <Hz>` | 最大フォルマント（base / target 共通。2000〜10000。既定: 5500） |
| `--acoustic-model <名前>` / `--dictionary <名前>` | MFA の音響モデル / 辞書（既定: `japanese_mfa`） |
| `--overwrite` | 保存先があれば上書きする（付けなければ、あるときは中止する） |

`convert <tcmorph のアンカー JSON>` のオプションは次のとおりです。

| オプション | 内容 |
|---|---|
| `--out <JSON>` | 保存するセッション（必須） |
| `--base <音声>` / `--target <音声>` | base / target の音声（両方を指定する。指定すると `manifest.json` は読まない） |
| `--overwrite` | 保存先があれば上書きする（付けなければ、あるときは中止する） |

`export <セッション>` のオプションは次のとおりです。GUI の「連続書き出し...」と同じ処理で、ファイル名と JSON も
同じです。

| オプション | 内容 |
|---|---|
| `--count <本数>` | 書き出す本数（両端を含む。2〜101。必須） |
| `--out <フォルダ>` | 出力先（無ければ作る。必須） |
| `--prefix <接頭辞>` | ファイル名の接頭辞（既定: base と target のファイル名をつないだもの） |
| `--sweep <軸,...>` | 変化させる軸（`tx`, `fx`, `fo`, `sl`, `ap` をコンマで区切る。既定: `all`） |
| `--fixed <軸=率,...>` | 変化させない軸の率（既定: 0） |
| `--no-manifest` | 率とアンカーを残す JSON（`<接頭辞>_batch.json`）を書かない |
| `--overwrite` | 同じ名前のファイルがあれば上書きする（付けなければ、あるときは中止する） |
| `--envelope-mix <average\|ot>` | 包絡の混ぜ方。`average` は平均のまま、`ot` は範囲の中だけ OT で補間する（GUI の「OT で補間」。既定: `average`） |
| `--ot-gamma <γ>` | OT の質量 = パワー^γ（0.05〜1。既定: 0.3。`--envelope-mix ot` と一緒に使う） |
| `--ot-range <下限>-<上限>` | OT で補間する範囲 [Hz]（300〜24000。既定: `5000-12000`。`--envelope-mix ot` と一緒に使う） |
| `--swap` | base と target を入れ替えて書き出す（率 0 が元の target になる。入力のセッションは変えない） |

- 共通: `--version` / `-h`, `--help`（サブコマンドの後に書けば、そのサブコマンドの説明）。
- 値は `--base a.wav` と `--base=a.wav` のどちらでも書けます。相対パスは起動したときのカレントディレクトリが基準です。
- 音声のパスを含むセッションと `--base` / `--target` は同時に指定できません。
- 自動のアンカーは誤ることがあるので、`auto-anchors` で保存したセッションを GUI で開いて確かめてから `export` に渡すと
  安心です。確かめずに続けて書き出すなら、`&&` でつなぎます（`auto-anchors ... --out auto.json && export auto.json ...`）。
- `convert` は、`--base` / `--target` が無ければ、同じフォルダの `manifest.json`（`mat2json.py` の出力）の
  `fileFullPath` と `fileName` から音声を決めます（相対パスは `manifest.json` のフォルダが基準）。`manifest.json` が
  無いとき、音声が見つからないとき、音声の標本化周波数が manifest と違うときは、エラーで止まります。
  WORLD パラメータ（npy）は使わず、音声はこのアプリで解析し直します。
- `export --envelope-mix ot` のファイル名には、γ と範囲の印が入ります（例: `<接頭辞>_ot030_5000-12000_r050.wav`）。
  平均のものとは名前が違うので、同じ出力先に両方を書き出して聴き比べられます。OT は1本ごとの合成に 2〜3 倍の
  時間がかかります。sl を 0 か 1 に固定すると OT は効かないので、警告が出ます。
- `export --swap` は、音声とアンカーの base と target を入れ替えてから書き出します。既定の接頭辞も target-base の順に
  なり、書き出しの JSON の `swapped_from` に入れ替える前の音声のパスが残ります。`--fixed` の率も入れ替えた向きで
  読みます（`--swap --fixed tx=0.3` は、元の向きの tx = 0.7）。入れ替えた率 r の音は、元の向きの率 1 − r の音と
  同じです（実データで、OT のときも含めて確かめました）。変わるのは、率の向きとファイル名、JSON の base / target です。
- 生成の設定と音素列（`auto_anchors`）や、元の `.mat` のパスなど manifest の情報（`tcmorph`）はセッションに残り、
  `export` の書き出しの JSON にも引き継がれます。
- 経過は標準エラーに、作ったファイルの一覧は標準出力に出ます。終了コードは 0 = 成功、1 = 処理の失敗、
  2 = 引数の誤り、130 = Ctrl+C で中止です。
- v26.11.17 までの `batch` サブコマンドはなくしました。`batch --auto-anchors --save-session` は `auto-anchors`、
  `batch --session anchors.json --save-session` は `convert`、`batch --count ... --out ...` は `export` に当たります。
- フォント・ライセンスの条文・音声解析の環境（`.env`）はカレントディレクトリ（とその1つ上）から探すので、
  配布物のルート（開発時はリポジトリのルート）をカレントディレクトリにして起動してください。
