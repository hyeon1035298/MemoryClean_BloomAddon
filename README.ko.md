# 메모리 정리 (Bloom 애드온)

[English](README.md)

[Bloom](https://github.com/hyeon1035298/Bloom) 런처용 애드온입니다. 작업표시줄 시계 옆에 작은 RAM 모듈을 두고 메모리 사용률이나 CPU 사용률, 또는 둘 다를 아이콘 두 개로 보여 줍니다. 아이콘을 우클릭하면 메모리와 CPU가 바뀝니다. 누르면 탭 두 개짜리 창이 열립니다.

- **정리**: 지금 메모리를 비우고, 비울 영역을 고르고, 사용률이 일정 비율을 넘거나 몇 분마다 자동으로 정리합니다.
- **프로그램**: 실행 중인 프로그램을 메모리나 CPU 순으로 보여 주고 끝낼 수 있습니다. 목록과 CPU는 이 탭이 열려 있을 때만 읽습니다.

제외 목록에 넣은 프로그램은 정리할 때 메모리를 비우지 않습니다. Bloom은 처음부터 들어 있습니다. 톱니바퀴는 설정(표시 항목과 아이콘 모양, 아일랜드, 영역, 자동 정리, 제외, 도우미)을 엽니다.

Windows 11 기본 작업표시줄에서도, [PillTaskbar](https://github.com/hyeon1035298/PillTaskbar_BloomAddon)로 바꾼 작업표시줄에서도 동작합니다. PillTaskbar의 애드온 런타임과 호스트가 이 애드온을 탐색기에 올리며, PillTaskbar 애드온을 켤 필요는 없습니다.

PillTaskbar와 자동 숨김 작업표시줄을 쓰면, 작업표시줄이 들어가 있는 동안 PillTaskbar 아일랜드의 왼쪽이나 오른쪽에 작은 아일랜드로도 아이콘을 볼 수 있습니다. 그 아일랜드의 클릭은 설정에 따라 통과하거나 작업표시줄을 꺼냅니다.

## 필요한 것

- Windows 11, 64비트
- Bloom 0.19.0 이상
- 처음 켤 때 인터넷 연결(Microsoft 기호 파일을 받아 필요한 위치만 남깁니다)

## 설치

1. [Releases](https://github.com/hyeon1035298/MemoryClean_BloomAddon/releases)에서 `MemoryClean_BloomAddon-<버전>.zip`을 받습니다.
2. `%LOCALAPPDATA%\Bloom\user-addons\memory-clean\`에 풉니다. 폴더 이름은 `memory-clean`이어야 합니다.
3. Bloom을 다시 시작하고 설정 › 애드온에서 "메모리 정리"를 켭니다.

지울 때는 도우미를 설치했다면 먼저 제거하고(톱니바퀴 › 도우미), 애드온을 끈 뒤 폴더를 지웁니다.

### 도우미 (선택)

관리자 권한 없이는 내 프로그램들의 작업 집합만 줄입니다. 대기 목록, 수정된 페이지 목록, 파일 캐시 같은 시스템 전체 목록을 비우려면 관리자 권한이 필요하고, 도우미가 그 일을 맡습니다.

- 설치할 때 관리자 권한을 한 번 묻습니다(UAC 한 번). `MemoryCleanHelper.exe`를 `%ProgramFiles%\Bloom Add-ons\MemoryClean\`에 복사하고, 애드온이 요청할 때만 높은 권한으로 실행되는 예약 작업을 `\Bloom\` 아래에 등록합니다.
- 요청 하나(이 영역 비우기, 이 프로그램 끝내기)만 하고 바로 끝납니다. 결과는 `%LOCALAPPDATA%\Bloom\addons\memory-clean\helper-result.json`에 씁니다.
- 제거하면(UAC 한 번 더) 예약 작업을 지우고, 다른 사용자의 작업이 남아 있지 않으면 폴더도 지웁니다.

## 설정

Bloom의 애드온 설정 화면에는 테마(`pill.theme`: Windows 작업표시줄 테마·다크·라이트), 유리 효과(`pill.glass`), 새로 고침 간격(`widget.refreshSeconds`, 1~10초)이 있습니다. 테마와 유리 효과는 Bloom이 자기 값으로 맞춥니다. 나머지(영역, 자동 정리, 제외, 아이콘 모양)는 애드온 창에서 바꾸며 `%LOCALAPPDATA%\Bloom\addons\memory-clean\prefs.json`에 저장됩니다.

## 빌드

[README.md](README.md#build-from-source)를 보세요. 이 저장소와 PillTaskbar_BloomAddon을 나란히 받고 `bash build.sh`를 실행합니다.

## 라이선스

GNU General Public License 3.0 only(`LICENSE`). 출처와 라이선스 전문은 `THIRD_PARTY.md`에 있습니다. 보증은 없습니다. 탐색기(explorer.exe) 안의 코드를 바꾸고 프로그램을 끝내거나 시스템 메모리 목록을 비우므로 본인 책임으로 쓰세요. Microsoft, Mem Reduct와 관계없는 프로젝트입니다.
