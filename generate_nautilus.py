#!/usr/bin/env python3
import os
import re

def parse_files(cmd_file, ext_file):
    apps = {}
    
    # 1. Read and clean the extensions file
    if os.path.exists(ext_file):
        with open(ext_file, 'r', encoding='utf-8') as f:
            current_app = None
            for line in f:
                line = line.strip()
                if line.startswith("App: "):
                    current_app = line.replace("App: ", "").strip()
                    if current_app not in apps:
                        apps[current_app] = {'exe_path': '', 'actions': [], 'exts': []}
                elif line.startswith("Extensions: ") and current_app:
                    exts_raw = line.replace("Extensions: ", "").strip()
                    if exts_raw == "* (All Files)":
                        apps[current_app]['exts'] = ['*']
                    else:
                        # Clean extensions from any commas or overlapping spaces
                        parts = re.split(r'[, ]+', exts_raw)
                        cleaned = [p.lower() for p in parts if p.startswith('.')]
                        apps[current_app]['exts'] = cleaned

    # 2. Read the commands file
    if os.path.exists(cmd_file):
        with open(cmd_file, 'r', encoding='utf-8') as f:
            current_app = None
            for line in f:
                line = line.strip()
                if line.startswith("App: "):
                    current_app = line.replace("App: ", "").strip()
                    if current_app not in apps:
                        apps[current_app] = {'exe_path': '', 'actions': [], 'exts': []}
                elif line.startswith("ExePath: ") and current_app:
                    apps[current_app]['exe_path'] = line.replace("ExePath: ", "").strip()
                elif line.startswith("Action: ") and current_app:
                    action_name = line.replace("Action: ", "").strip()
                    apps[current_app]['actions'].append({'name': action_name, 'cmd': ''})
                elif line.startswith("Command: ") and current_app:
                    cmd = line.replace("Command: ", "").strip()
                    apps[current_app]['actions'][-1]['cmd'] = cmd

    return apps

def generate_scripts(apps):
    out_dir = os.path.expanduser("~/.local/share/nautilus-python/extensions")
    os.makedirs(out_dir, exist_ok=True)

    for app_name, data in apps.items():
        # Generate the filename and class name
        app_lower = app_name.lower().replace(" ", "_")
        app_class = re.sub(r'[^a-zA-Z0-9]', '', app_name)
        out_file = os.path.join(out_dir, f"{app_lower}.py")

        # Convert the extensions list into a Python Set format
        if not data['exts']:
            exts_set = "{'*'}"
        else:
            exts_set = "{" + ", ".join(f"'{e}'" for e in data['exts']) + "}"
        
        # Nautilus extension code template
        py_code = f'''import os
import subprocess
import shlex
import gi

try:
    gi.require_version('Nautilus', '4.0')
except ValueError:
    gi.require_version('Nautilus', '4.1')

from gi.repository import GObject, Nautilus

class {app_class}MenuProvider(GObject.GObject, Nautilus.MenuProvider):
    def __init__(self):
        super().__init__()
        self.valid_exts = {exts_set}
        self.actions = {data['actions']}

    def get_file_items(self, *args):
        files = args[-1]
        if not files:
            return []

        for file in files:
            if file.is_directory():
                return []
            
            filename = file.get_name()
            ext = os.path.splitext(filename)[1].lower()
            
            if '*' not in self.valid_exts and ext not in self.valid_exts:
                return []

        main_item = Nautilus.MenuItem(name='WineMenu::MainMenu', label='winemenu')
        main_submenu = Nautilus.Menu()
        main_item.set_submenu(main_submenu)

        app_item = Nautilus.MenuItem(name='WineMenu::{app_class}', label='{app_name}')
        app_submenu = Nautilus.Menu()
        app_item.set_submenu(app_submenu)
        main_submenu.append_item(app_item)

        for idx, action in enumerate(self.actions):
            item = Nautilus.MenuItem(name=f'{app_class}Action::{{idx}}', label=action['name'])
            item.connect('activate', self.execute_wine_app, files, action['cmd'])
            app_submenu.append_item(item)

        return [main_item]

    def execute_wine_app(self, menu, files, raw_cmd):
        for file in files:
            linux_filepath = file.get_location().get_path()
            if not linux_filepath:
                continue
            
            # Convert Linux path to Windows path using winepath
            try:
                win_filepath = subprocess.check_output(['winepath', '-w', linux_filepath]).decode('utf-8').strip()
            except subprocess.CalledProcessError:
                win_filepath = linux_filepath

            working_dir = os.path.dirname(linux_filepath)
            
            # Build the command and substitute the file path 
            cmd_string = raw_cmd.replace('%1', win_filepath).replace('"%1"', f'"{{win_filepath}}"')
            parsed_cmd = shlex.split(cmd_string)
            
            # Ensure the command is executed via wine
            if parsed_cmd[0].lower() != 'wine':
                final_cmd = ['wine'] + parsed_cmd
            else:
                final_cmd = parsed_cmd
                
            final_cmd[1] = os.path.expanduser(final_cmd[1])

            subprocess.Popen(final_cmd, cwd=working_dir)
'''
        # Save the file directly to the Nautilus extensions path
        with open(out_file, 'w', encoding='utf-8') as f:
            f.write(py_code)
        
        print(f"[+] Extension successfully created and exported to: {out_file}")

if __name__ == "__main__":
    print("--- [ Reading Data and Generating Nautilus Extensions ] ---")
    apps_data = parse_files("extracted_commands.txt", "extracted_extensions.txt")
    
    if not apps_data:
        print("[!] No data found! Make sure the txt files exist.")
    else:
        generate_scripts(apps_data)
        print("\n[+] Operation complete! Restart Nautilus to apply changes using the command:")
        print("nautilus -q")