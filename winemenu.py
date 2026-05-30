import os
import subprocess
import gi

try:
    gi.require_version('Nautilus', '4.0')
except ValueError:
    gi.require_version('Nautilus', '4.1')

from gi.repository import GObject, Nautilus

class WineMenuProvider(GObject.GObject, Nautilus.MenuProvider):
    def __init__(self):
        super().__init__()
        self.valid_exts = {
            '.001', '.7z', '.arj', '.bz', '.bz2', '.cab', '.gz', '.iso', 
            '.jar', '.lha', '.lz', '.lzh', '.rar', '.tar', '.taz', '.tbz', 
            '.tbz2', '.tgz', '.tlz', '.txz', '.tzst', '.uu', '.uue', '.xxe', 
            '.xz', '.z', '.zip', '.zipx', '.zst', '.rev'
        }
        self.winrar_exe = os.path.expanduser("~/.wine/drive_c/Program Files/WinRAR/WinRAR.exe")

    def get_file_items(self, *args):
        files = args[-1]
        if not files:
            return []

        for file in files:
            if file.is_directory():
                return []
            
            filename = file.get_name()
            ext = os.path.splitext(filename)[1].lower()
            
            if ext not in self.valid_exts:
                return []

        main_item = Nautilus.MenuItem(name='WineMenu::MainMenu', label='winemenu')
        main_submenu = Nautilus.Menu()
        main_item.set_submenu(main_submenu)

        winrar_item = Nautilus.MenuItem(name='WineMenu::WinRAR', label='WinRAR')
        winrar_submenu = Nautilus.Menu()
        winrar_item.set_submenu(winrar_submenu)
        main_submenu.append_item(winrar_item)

        ext_here = Nautilus.MenuItem(name='WinRAR::ExtractHere', label='Extract Here')
        ext_here.connect('activate', self.execute_winrar, files, 'extract_here')
        winrar_submenu.append_item(ext_here)

        ext_to = Nautilus.MenuItem(name='WinRAR::ExtractTo', label='Extract to...')
        ext_to.connect('activate', self.execute_winrar, files, 'extract_to')
        winrar_submenu.append_item(ext_to)

        open_winrar = Nautilus.MenuItem(name='WinRAR::Open', label='Open with WinRAR')
        open_winrar.connect('activate', self.execute_winrar, files, 'open')
        winrar_submenu.append_item(open_winrar)

        return [main_item]

    def execute_winrar(self, menu, files, action):
        for file in files:
            linux_filepath = file.get_location().get_path()
            if not linux_filepath:
                continue
            
            # الحل هنا: تحويل مسار لينكس إلى مسار ويندوز باستخدام winepath
            try:
                win_filepath = subprocess.check_output(['winepath', '-w', linux_filepath]).decode('utf-8').strip()
            except subprocess.CalledProcessError:
                win_filepath = linux_filepath # كحل بديل في حال فشل التحويل

            working_dir = os.path.dirname(linux_filepath)
            
            # بناء الأوامر باستخدام المسار المترجم
            if action == 'extract_here':
                cmd = ['wine', self.winrar_exe, 'x', win_filepath]
            elif action == 'extract_to':
                cmd = ['wine', self.winrar_exe, 'x', '-ad', win_filepath]
            elif action == 'open':
                cmd = ['wine', self.winrar_exe, win_filepath]
            
            subprocess.Popen(cmd, cwd=working_dir)
