; pigeon installer: per user, no admin. pigeon updates itself by swapping its own exe,
; which needs a folder the user can write to, so not Program Files.
; Build: makensis /DVERSION=v1.1 pigeon.nsi  (pigeon.exe next to this file)

Unicode true
!include MUI2.nsh

!ifndef VERSION
  !define VERSION "dev"
!endif
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\pigeon"

Name "pigeon"
OutFile "pigeon-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\pigeon"
RequestExecutionLevel user
SetCompressor /SOLID lzma

!define MUI_ICON "pigeon.ico"
!define MUI_UNICON "pigeon.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\pigeon.exe"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section
  nsExec::Exec 'taskkill /im pigeon.exe /f'   ; a running pigeon locks its exe
  Sleep 500
  SetOutPath "$INSTDIR"
  File "pigeon.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\pigeon.lnk" "$INSTDIR\pigeon.exe"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "pigeon"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "beeprat"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\pigeon.exe"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Section "un.pigeon" ; always
  SectionIn RO
  nsExec::Exec 'taskkill /im pigeon.exe /f'
  Sleep 500
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "pigeon"
  Delete "$SMPROGRAMS\pigeon.lnk"
  Delete "$INSTDIR\pigeon.exe"
  Delete "$INSTDIR\pigeon.exe.old"   ; left by a self-update
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINST}"
SectionEnd

Section /o "un.Also delete my accounts and settings"
  RMDir /r "$APPDATA\pigeon"
SectionEnd
