# wmux

## 소개
`wmux`는 Windows 환경에서 동작하는 가벼운 터미널 멀티플렉서입니다.  
ConPTY를 기반으로 셸을 실행하고, 하나의 창 안에서 여러 `pane`을 나누어 동시에 작업할 수 있습니다.  
tmux와 비슷한 작업 흐름을 Windows GUI에 맞게 옮긴 형태로, 키보드 중심 제어와 마우스 조작을 함께 지원합니다.

주요 특징:

- 하나의 창에서 다중 pane 분할
- pane 포커스 이동, 내용 교환, 드래그 이동
- 스크롤백, 텍스트 선택, 복사/붙여넣기
- Direct2D 기반 렌더링
- 설정 창을 통한 글꼴, 배경색, 분할선 색, 입력 옵션 조정

## 실행
빌드 후 실행 파일은 보통 아래 경로에 생성됩니다.

- `build/Debug/wmux.exe`
- `build/Release/wmux.exe`

실행하면 기본적으로 하나의 셸 pane이 열립니다.

## 기본 조작
### Prefix 명령
기본 prefix는 `Ctrl+B`입니다.  
`Ctrl+B`를 누른 뒤 아래 키를 입력합니다.

- `%` 또는 `v`: 좌우 분할
- `"` 또는 `h`: 상하 분할
- `x`: 현재 pane 닫기
- `z`: 현재 pane 확대/복귀
- `o`: 설정 창 열기
- 방향키: pane 포커스 이동
- `Ctrl+B`: pane에 실제 `Ctrl+B` 전송

### 직접 단축키
prefix 없이 바로 사용할 수 있습니다.

- `Ctrl+방향키`: pane 포커스 이동
- `Alt+방향키`: pane 위치 이동
- `Alt+Shift+방향키`: pane 내용 교환
- `Alt+H`: 도움말 표시
- `Ctrl+A`: 전체 선택
- `Ctrl+C`: 선택 영역 복사, 선택이 없으면 SIGINT 전송
- `Ctrl+V`: 붙여넣기
- `Shift+PageUp / Shift+PageDown`: 스크롤백 이동

## 마우스 사용
- 좌클릭 드래그: 텍스트 선택
- 더블클릭: 단어 선택
- 우클릭: 선택이 있으면 복사, 없으면 붙여넣기
- 마우스 휠: 스크롤백 이동
- 스크롤바 드래그: 스크롤 위치 조정
- 분할선 드래그: pane 크기 비율 조절
- `Shift` + 좌클릭 드래그: pane 드래그 이동

## 설정
설정 창에서는 다음 항목을 조정할 수 있습니다.

- 글꼴과 글자 크기
- 창 너비와 높이
- 비활성 pane 어둡게 표시
- 배경색
- 분할선 색
- prefix 표시 오버레이
- 스크롤 줄 수
- 유휴 효과 시간

설정은 실행 파일 옆의 `config.ini`에 저장됩니다.

## 사용 예
예를 들어 왼쪽 pane에서는 서버를 실행하고, 오른쪽 pane에서는 로그를 보거나 Git 작업을 할 수 있습니다.  
여러 프로젝트 폴더를 드롭해서 새 pane을 열고, 필요한 pane만 확대해서 집중하는 방식으로 쓰기 좋습니다.

![image](https://github.com/codesafe/WMUX/blob/main/pic/%EF%BC%91.png)
![image](https://github.com/codesafe/WMUX/blob/main/pic/%EF%BC%92.png)
![image](https://github.com/codesafe/WMUX/blob/main/pic/%EF%BC%93.png)



