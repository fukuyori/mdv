# mdv

C++ と Qt Widgets で作られたシンプルな Markdown ビュワー/エディタです。

[English](README.md) | 日本語 | [変更履歴](CHANGELOG.ja.md)

## スクリーンショット

エディタとライブプレビュー:

![mdv のエディタとプレビュー](images/screenshot1.png)

ビュワーモード(`-v`、編集ペイン非表示):

![mdv のビュワーモード](images/screenshot2.png)

## 機能

- 左ペインで Markdown を編集
- Markdown 向けの入力補助: 改行時の自動インデント、リスト・引用マーカー
  (箇条書き・番号付き・チェックボックス・引用)の自動継続、Shift+Enter で
  プレーン改行、Tab / Shift+Tab で選択行のインデント/解除
- GitHub Flavored Markdown のライブプレビュー(表・タスクリスト・打ち消し線・
  オートリンク・生 HTML)。Qt WebEngine + [md4c](https://github.com/mity/md4c) で描画
- プレビューのコンテキストメニューから選択範囲をコピー。コードブロックは
  ブロック上のボタンで全体を一括コピー
- エディタとプレビューの双方向スクロール同期(見出しアンカー方式)
- 左側のアウトラインで見出しを一覧・ジャンプ
- プレビュー内の見出しアンカーリンク(`#section`)に対応。
  外部リンクはシステムブラウザで開く
- Markdown ファイルの新規作成・読み込み・保存・名前を付けて保存
- タブ機能: 複数の Markdown ファイルを同時に編集。各タブは独立した
  エディタ・アウトライン・プレビューを持つ。コマンドライン引数や
  ウィンドウへのドラッグ&ドロップで複数ファイルを一度に開ける。
  すでに開いているファイルを開こうとした場合は既存のタブに切り替える
- 複数ウインドウ対応: タブを右クリックして「新しいウインドウで開く」、
  またはタブをタブバーの外へドラッグして新しいウインドウとして切り離せる。
  タブを別の mdv ウインドウへドロップするとそちらへ移動する
- タブが開いているファイルがディスク上で変更された(他のプログラムに
  よる編集など)ことを検知し、再読み込みするか確認
- 追従表示モード(`-f`): ファイルへの追記を自動で再読み込みし、編集ペインを
  隠したプレビューの最終行を常に画面下端へ表示
- 文字コードの安全性: UTF-8 として読み込み(BOM 付き UTF-16/32 は自動判別し、
  保存時も同じエンコーディングを維持)。正しくデコードできないファイルは
  開く前・保存前に警告
- 10 MB を超えるファイルを開く前に確認ダイアログを表示
- 最近開いたファイルメニュー
- 検索と置換。マッチはエディタとプレビューの両方でハイライト
  (全マッチをマークし、現在のマッチは強調表示+自動スクロール)
- 元に戻す・やり直し・切り取り・コピー・貼り付け
- クリップボードの画像や画像ファイルを Markdown の画像リンクとして貼り付け
- Mermaid 図とインライン・ブロック形式の LaTeX 数式をプレビューに描画。
  オフライン用ライブラリをアプリに同梱
- フェンスコードブロックを言語に応じてシンタックスハイライト
- GitHub 形式の注記・ヒント・重要・警告・注意アラートを描画
- ライト・ダーク・セピアのテーマ切り替え
- エディタ・アウトライン・プレビューの文字サイズ変更
- エディタとプレビューのフォント選択
- ローカルの [Ollama](https://ollama.com) サーバーによるプレビューの翻訳。
  プレビュー上部のボタンで「原文」「対訳」(原文と訳文を交互に表示)「翻訳」を
  切り替え([翻訳](#翻訳) を参照)
- UI 言語の切り替え(英語/日本語)
- プレビュー横の細いトグルまたは表示メニューで編集ペインの表示/非表示を切り替え

## Mermaid と数式

Mermaid 図は、言語名を `mermaid` にしたフェンスコードブロックで記述します。

````markdown
```mermaid
flowchart LR
    A[Markdown] --> B[プレビュー]
```
````

LaTeX 数式は、インラインでは `$...$`、独立した数式では `$$...$$` を使用します。

```markdown
オイラーの等式は $e^{i\pi} + 1 = 0$ です。

$$
\int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi}
$$
```

Mermaid と KaTeX は mdv に同梱されるため、インターネット接続なしで利用できます。
Mermaid の構文に誤りがある場合はエラーメッセージと元のソースを表示し、数式の
構文に誤りがある場合もプレビュー全体を壊さず入力内容を表示します。

## シンタックスハイライトとアラート

開始フェンスの後へ言語名を付けると、コードブロックをハイライトします。

````markdown
```cpp
#include <iostream>

int main() {
    std::cout << "Hello\n";
}
```
````

GitHub 形式のアラートは、引用ブロックの先頭行へ `NOTE`、`TIP`、`IMPORTANT`、
`WARNING`、`CAUTION` のいずれかを記述します。

```markdown
> [!NOTE]
> 補足として役立つ情報です。

> [!WARNING]
> ファイルを上書きする前に保存先を確認してください。
```

同梱の Highlight.js 共通言語ビルドとライト・ダーク・セピア用スタイルにより、
オフラインで動作します。アラートの見出しは選択中のUI言語で表示します。

## ビルド

WebEngine、WebChannel、Network モジュールを含む Qt が必要です(macOS では `brew install qt`)。

```sh
cmake -S . -B build
cmake --build build
```

## 実行

macOS:

```sh
open build/mdv.app
```

その他のプラットフォーム:

```sh
./build/mdv
```

引数に1つ以上のファイルを渡すと、それぞれが別タブとして開きます:

```sh
open -a mdv README.md CHANGELOG.md                        # macOS(インストール済みアプリ)
build/mdv.app/Contents/MacOS/mdv README.md CHANGELOG.md   # macOS(バイナリ直接実行)
./build/mdv README.md CHANGELOG.md                        # その他
```

ウィンドウにファイルをドラッグ&ドロップしてタブとして開くこともできます。
macOS では Finder の「このアプリケーションで開く」や、Dock アイコンへの
ドロップでもファイルを開けます。

編集ペインを隠したビュワーモードで起動:

```sh
open build/mdv.app --args -v   # macOS
./build/mdv -v                 # その他
```

ビュワーモードでは、プレビュー左端の細いトグルから編集ペインを再表示できます。

`tail -f` のようにファイルの追記を自動で再読み込みし、最終行をプレビューの
下端へ表示し続ける追従表示モード:

```sh
open build/mdv.app --args -f log.md   # macOS
./build/mdv -f log.md                 # その他
```

追従表示モードは読み取り専用です。編集ペインを非表示に固定し、外部の書き込みを
上書きしないよう保存・編集操作を無効化します。同じウインドウで追加して開いた
ファイルも同様に追従表示します。
初回以降は新たに追記されたバイトだけを読み込みます。表示範囲を末尾10,000行かつ
ソースデータ最大8 MiBに制限するため、ファイルが増えてもメモリ使用量は増加しません。
切り詰め・同一ファイルへの上書き・ファイル置換やローテーションを検出した場合は、
新しいファイルの末尾から有限バッファを作り直します。

「開く」「名前を付けて保存」ダイアログの初期ディレクトリはカレントディレクトリ
です(Finder から起動した場合はホーム)。以後は最後に使ったディレクトリを
記憶します。「名前を付けて保存」は開いているファイルの名前だけを引き継ぐため、
開いたファイルの場所に保存先が引きずられることはありません。

## 翻訳

プレビューはローカルの [Ollama](https://ollama.com) サーバーで翻訳できます。
Ollama をインストールし、モデルを取得(例: `ollama pull translategemma`)して、
サーバーが起動していることを確認してください。

プレビュー上部のボタン(または「翻訳」メニュー)で表示を切り替えます:

- **原文** - 通常のプレビュー
- **対訳** - 原文の各ブロックの直下に訳文を表示
- **翻訳** - 訳文のみを表示

「翻訳」→「翻訳の設定」で、エンドポイント(既定 `http://127.0.0.1:11434`)、
モデル(インストール済みモデルを自動で一覧表示)、翻訳先言語、
同時リクエスト数(1〜8。2 以上を活かすにはサーバー側も
`OLLAMA_NUM_PARALLEL` を 2 以上にして起動する必要があります)を設定できます。

翻訳は現在表示中の位置から読み順に行われ、訳文は到着したブロックから順次
表示されます。訳文はブロック単位でキャッシュされるため、編集時は変更した
ブロックだけが再翻訳されます。翻訳に失敗したブロックは対訳表示で
「(翻訳失敗)」マーカー付きの原文表示となり、残りの翻訳は継続します。

## プロジェクト構成

```
src/main.cpp        アプリケーション本体(ウィンドウ、エディタ、プレビュー、同期)
third_party/md4c/   ベンダリングした md4c Markdown パーサ(MIT ライセンス)
third_party/mermaid ベンダリングした Mermaid 図レンダラー(MIT ライセンス)
third_party/katex/  ベンダリングした KaTeX 数式レンダラーとフォント(MIT ライセンス)
third_party/highlightjs/ ベンダリングした Highlight.js (BSD-3-Clause)
resources/          アプリアイコンのソースと macOS アイコンセット
scripts/            macOS のリリース・署名・アイコン生成スクリプト
tools/icon_renderer アイコンスクリプトが使う SVG→PNG 変換ヘルパー
```

### プレビューの仕組み

エディタのテキストを md4c(GitHub 方言と LaTeX 数式拡張)で HTML に変換し、
`QWebEngineView` に
流し込みます。テーマ CSS を含むテンプレートページは一度だけロードし、以後は
ページ内容だけを 120ms のデバウンス付きで差し替えるため、入力中にプレビューが
ちらついたりスクロール位置を失ったりしません。スクロール同期は両ペインの
見出しを対応付けて区間内を線形補間する方式で、プレビュー側のスクロールは
`QWebChannel` 経由でエディタに通知されます。同梱の KaTeX が md4c の数式要素を、
同梱の Mermaid が `mermaid` コードフェンスを非同期に描画します。文書が再編集
された場合は古い図の描画結果を破棄します。残りのコードフェンスは内容の更新ごとに
同梱の Highlight.js で処理し、GitHub 形式のアラート引用も同じ DOM 処理で分類・
装飾します。プレビュー内のリンクをクリックしても
ページ遷移は起きず、http/https/mailto はシステムブラウザで開き、それ以外の
スキームはブロックされます。

## リリースビルド

macOS で Release ビルドのアプリバンドルを作成:

```sh
scripts/macos_build_release.sh
```

`resources/icon.svg` から macOS アプリアイコンを再生成:

```sh
scripts/macos_generate_icon.sh
```

署名・DMG 作成・公証(ノータライズ)・ステープル:

```sh
scripts/macos_sign_dmg_notarize.sh
```

DMG は `dist/macos/mdv-<version>-macos-arm.dmg` として出力されます。

公証スクリプトは既定で `notarytool` という名前で保存された notarytool
プロファイルを使います。必要に応じて環境変数で上書きできます:

```sh
CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)" \
NOTARY_PROFILE="notarytool" \
scripts/macos_sign_dmg_notarize.sh
```

`scripts/macos/entitlements.plist` の Hardened Runtime エンタイトルメントには、
Qt WebEngine(Chromium)が必要とする JIT 関連のキーが含まれています。

Windows の Release ビルド:

```powershell
.\scripts\build-windows.ps1
```

Release ビルドを作成し、`windeployqt` で Qt ランタイムを
`dist\mdv-windows-x64` に配置します。

Inno Setup で Windows インストーラーを作成:

```powershell
.\scripts\package-windows-inno.ps1
```

インストーラースクリプトは `CMakeLists.txt` からバージョンを読み取り、
`dist\mdv-windows-x64` にある既存のペイロードを使います。その後
`build-inno-installer\mdv.iss` を生成し、`dist\mdv-<version>-windows-x64.exe` を
出力します。先に `.\scripts\build-windows.ps1` を実行してください。
パッケージ作成時に明示的に再ビルドしたい場合だけ `-Build` を指定します。
Inno Setup を実行せず `.iss` だけ生成する場合は `-GenerateOnly` を指定してください。

Windows の電子署名(Authenticode):

```powershell
$env:CODESIGN_CERT = "C:\path\to\cert.pfx"
$env:CODESIGN_CERT_PASSWORD = "..."
.\scripts\build-windows.ps1 -Sign
.\scripts\package-windows-inno.ps1 -Sign
```

どちらのスクリプトも `-Sign` を受け付け、証明書は環境変数
`CODESIGN_CERT` から取得します。`CODESIGN_CERT` には `.pfx` のパス、
証明書ストア内の SHA1 サムプリント、またはサブジェクト名を指定できます。
`build-windows.ps1 -Sign` は配置後の `mdv.exe` に署名し、
`package-windows-inno.ps1 -Sign` はペイロードの `mdv.exe`、インストーラー、
アンインストーラーに署名します。任意の環境変数:
`CODESIGN_CERT_PASSWORD`(証明書ファイルのパスワード)、
`CODESIGN_TIMESTAMP_URL`(RFC 3161 サーバー、既定は
`http://timestamp.digicert.com`)、`CODESIGN_DIGEST`(既定は `sha256`)、
`CODESIGN_CSP` と `CODESIGN_KEY_CONTAINER`(ハードウェアトークン用)、
`SIGNTOOL`(`signtool.exe` のパス。`-SignToolPath` でも指定可)。
