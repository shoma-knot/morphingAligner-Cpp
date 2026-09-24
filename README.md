# morphingAligner

2つの音声（base / target）のスペクトル包絡を並べて表示し、時間軸・周波数軸の対応（アンカー）を
打って、その対応にもとづく音声モーフィングを行う GUI アプリです。

- スペクトル包絡の表示（WORLD）、時間・周波数アンカーの編集
- アンカーにもとづくモーフィングと再生・WAV 保存（tcmorph）
- フォルマントの表示と、音素アライメント（Montreal Forced Aligner）にもとづくアンカーの自動生成
  … この2つは追加のセットアップが必要です（下の「音声解析のセットアップ」）

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
- セットアップはやり直せます（同じスクリプトをもう一度実行）。環境を作り直したいときは
  `.env` フォルダを削除してから実行してください。
- フォルダのパスや Windows のユーザー名に日本語などの ASCII 以外の文字が含まれていると、
  音素アライメントが失敗することがあります（ログに警告が出ます）。その場合は `C:\tools\` などの
  ASCII だけのパスに配布物を置いてください。
- MFA のモデルはユーザーのフォルダ（`Documents/MFA`）に保存されます。
- 使わなくなったら、`.env` フォルダと `Documents/MFA` フォルダを削除すれば元に戻ります。

## 基本的な使い方

1. 左パネルの Base / Target の「読み込む」で音声を開きます（wav / flac / mp3 / ogg）。
2. スペクトログラムを左クリックすると時間アンカーが追加され、上下の線をドラッグして対応を合わせます。
   Ctrl を押しながらだと周波数アンカーの操作になります（「アンカー: n」の横の (?) に操作の一覧）。
3. 音声解析をセットアップ済みなら、書き起こし（例: 「はい」）を入力して「音素アライメント」を押し、
   「アンカー自動生成」で音素境界とフォルマントにもとづくアンカーを打てます。
4. 「モーフィング」タブで率を動かすと、モーフィングした音声を合成・再生できます。
5. 作業内容は「セッション保存」で JSON に保存できます。

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
