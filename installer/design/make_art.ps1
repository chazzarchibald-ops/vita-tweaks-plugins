# Makes the installer's bubble and LiveArea images (sce_sys) with TwArt.cs.
# Run from anywhere in Windows PowerShell: .\make_art.ps1
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$installer = Split-Path -Parent $here
Add-Type -Path (Join-Path $here 'TwArt.cs') -ReferencedAssemblies System.Drawing
[TwArt]::Fonts((Join-Path $installer 'font\Righteous-Regular.ttf'), (Join-Path $installer 'font\Poppins-Regular.ttf'))
$contents = Join-Path $installer 'sce_sys\livearea\contents'
New-Item -ItemType Directory -Force $contents | Out-Null
foreach ($image in @(
		@{ make = { [TwArt]::Icon() }; to = (Join-Path $installer 'sce_sys\icon0.png') },
		@{ make = { [TwArt]::Background() }; to = (Join-Path $contents 'bg.png') },
		@{ make = { [TwArt]::Gate() }; to = (Join-Path $contents 'startup.png') })) {
	$bitmap = & $image.make
	[TwArt]::Save($bitmap, $image.to)
	"wrote {0} ({1} x {2}, {3} bytes)" -f $image.to.Substring($installer.Length + 1), $bitmap.Width, $bitmap.Height, (Get-Item $image.to).Length
	$bitmap.Dispose()
}
