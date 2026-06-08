# Reignited — C++ VST3 Reference Implementation

このフォルダは「WebのGrokとCmajorで作っている本家プラグイン」の**C++参考実装** です。

いつもはGitHub上でCmajorコードを細かく仕様決めながら進めてるけど、「いきなりここでC++で動くものを作って参考にしたい」というリクエストで作りました。

## 現在のコンセプト（2026-06-08 実想まとめより）

- **プラグイン名**: Reignited
- **コンセプト**: 「あの時の熱や勢いを取り戻す……かもしれない」エフェクター
- ギターやボーカルに挿して、ただEQをいじるだけで「なんかあの時の感じが戻ってきた」と思わせる。
- 4バンドEQ（Low / Mid / High / Presence） + **5つ目のノブ「Reignited」** が本命。
- Reignitedノブを回すと **EQの変化 + SSの強度（Long Glueなど） + MIX + 各帯域のGR感度** が連動して変化。
- 中央付近（50%くらい）で既に「かかり過ぎの一歩手前」。70%以上で本格的にエフェクター領域に突入。
- 用途は**ミック特化**（リアルタイム演奏は非優先）。
- SS2（またはSS1寄り）の「気持ちいい性格」をベースに、偶数次サチュやキラキラ感などを選択的に取り入れる。
- EQフラットでもReignitedの位置で音が変わることを「味」として設計。

## このC++実装の位置づけ

- **参考プロトタイプ** 。本家はCmajorで作るので、ここで得た「気持ちいい方向性」やパラメータの連動感をCmajor側にフィードバックするのに使う。
- JUCE 7 (CMake FetchContent) で実装。VST3 + Standalone でビルド可能。
- パラメータは5つ（APVTS使用）。GenericAudioProcessorEditorで即座に触れる。
- DSPは「まず動いて、耳で判断できる」レベルを目標にしている。

## 実装済みの主な挙動（v0.1）

- 4バンド・シリアルEQ（Low: LowShelf 140Hz / Mid: Peak (Bell) 620Hz / High: HighShelf 2.8kHz / Presence: Peak (Bell) 5.8kHz）
- Reignitedノブで以下のものが連動：
  - サチュレーション量（ドライブ + 軽い非対称波形整形 → even harmonics寄り）
  - Long Glue（ゆっくりしたエンベロープフォロワーによる積やかなバスグルー）
  - 自動的なEQ変化（Reignitedが上がるとMidを少しscooping、低域をtighten、Presenceをair寄りに）
  - 内部MIX（Reignitedが上がるほど「エフェクター」成分が増える）
- ノブのカーブは「最初は緩やか → 0.55〜0.6以降で急に効きが強くなる」方向で調整してある。
- ステレオ対応（L/Rで独立したフィルタ状態）。

## ビルド方法（Windows）

### 前提ツール

- CMake 3.22 以上（すでに入ってるはず）
- Git（JUCEをFetchContentで自動ダウンロードするため）
- **Visual Studio の C++ 開発環境**

  重要：このマシンでは「Visual Studio 18 2026」などのgeneratorはcmakeのリストには出てくるけど、実際のインスタンス（C++コンパイラ一式）が入っていないためビルドに失敗します。

  **対処**:
  1. 「Visual Studio Installer」を起動（スタートメニューで検索）
  2. 「Visual Studio 2026」または「2022」の横の「変更」をクリック
  3. 「C++によるデスクトップ開発」ワークロードにチェックを入れてインストール
  4. （おすすめ）「MSVC v143 - VS 2022 C++ x64/x86 ビルドツール」なども入るようにする

  インストール後、**「x64 Native Tools Command Prompt for VS 2026」**（または2022）からPowerShellを起動して作業するとPATHが正しく通る。

  または普通のPowerShellでも、cmakeがMSVCを見つけられるようになるはず。

### ビルド手順（一番簡単なStandaloneから）

```powershell
cd C:\Users\user\Reignited

# 構成（最初はこれで）
cmake -G "Visual Studio 18 2026" -S . -B build
# 2022をお使いなら
# cmake -G "Visual Studio 17 2022" -S . -B build

# ビルド（Releaseが軽くておすすめ）
cmake --build build --config Release
```

成功すると以下が生成されます:

```
build\Reignited_artefacts\Release\Standalone\Reignited.exe   ← DAWなしで即テスト可能
```

（この環境で先に試したところ、generatorエラーで止まったので、上記のC++ワークロード追加が必須です）

### VST3を追加したいとき

1. CMakeLists.txt を開いて `FORMATS Standalone` の行を以下のように変更:

   ```cmake
   FORMATS Standalone VST3
   ```

2. 再度構成＋ビルド:

   ```powershell
   cmake -G "Visual Studio 18 2026" -S . -B build
   cmake --build build --config Release
   ```

   VST3 SDK が必要と言われたら、Steinbergの公式サイトから "VST3 SDK" をダウンロードしてパスを教えるか、JUCEが自動で扱えるようになるまで待つ（最近のJUCEは一部自動対応が進んでいる）。

### トラブルシューティング

- `Generator ... could not find any instance of Visual Studio` → 上記のC++ワークロードが未インストール。Installerで追加。
- JUCEのダウンロードに時間がかかる → 初回だけ。ネットワークによる。
- 「cl.exe が認識されません」→ Native Tools Command Prompt から実行するか、Developer Command Prompt for VS を起動してから cmake する。
- Standaloneが起動しない → 依存DLLの問題は稀だが、Visual C++ Redistributable を最新にすると良い場合あり。

まずは **Standalone** で音を出してReignitedノブをぐるぐる回してみるのが最短路ートです。

## 使い方（テスト）

1. Standalone版を起動。
2. DAW（REAPER, Cubase, Studio Oneなど）にVST3をスキャン。
3. ギターやボーカル、ドラムバスなどに挿してReignitedノブを回してみる。
4. 4つのEQノブは「普通のEQ」として機能しつつ、Reignitedを上げると性格が変わるのを体感。

## 今後の調整ポイント（本家Cmajorにフィードバックしたいこと）

- EQの正確な周波数 / Q / フィルタ種類（Low/Mid/High/Presenceの最適値）
- SS2 / Long Glue のより正確な「気持ちいい」挙動の移植（Cmajorコードがあればここに参考として置きたい）
- 各帯域ごとのGR感度の違いの強弱
- カット方向の味（ブーストとのバランス）
- もっと高次のオーバーサンプリングや多段サチュが必要か

## メモ

- このコードは「いきなりC++で参考を作る」ためのもの。綺麗さより「耳で判断できる速さ」を優先。
- Cmajor側で同じコンセプトを固めたら、またここでC++版を最新の挙動に追従させるのもアリ。
- 質問・修正リクエスト・「この部分をもっとSS2っぽく」などの指示はいつでもどうぞ。

---

Reignited reference — made here as a quick C++ counterpart to the Cmajor GitHub collaboration.
