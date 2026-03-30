$ws = New-Object -ComObject WScript.Shell
$desktop = [Environment]::GetFolderPath('Desktop')
$sc = $ws.CreateShortcut("$desktop\Qt-DAB.lnk")
$sc.TargetPath = "D:\cloud\Dropbox\Projekte\DAB\qt-dab-master\qt-dab-master\build\start-qtdab.cmd"
$sc.WorkingDirectory = "D:\cloud\Dropbox\Projekte\DAB\qt-dab-master\qt-dab-master\build"
$sc.WindowStyle = 7
$sc.IconLocation = "D:\cloud\Dropbox\Projekte\DAB\qt-dab-master\qt-dab-master\build\Qt-DAB.exe,0"
$sc.Save()
