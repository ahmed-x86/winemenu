#!/usr/bin/env python3
import os
import re

# =========================================================
# 1. Knowledge Base for Complex Programs (COM Objects)
# =========================================================
COM_KNOWLEDGE_BASE = {
    "WinRAR": [
        {"name": "Extract Here", "cmd": '"{exe_path}" x "{file_path}"'},
        {"name": "Extract to...", "cmd": '"{exe_path}" x -ad "{file_path}"'},
        {"name": "Open with WinRAR", "cmd": '"{exe_path}" "{file_path}"'}
    ],
    "7-Zip": [
        {"name": "Extract Here", "cmd": '"{exe_path}" x "{file_path}"'},
        {"name": "Open Archive", "cmd": '"{exe_path}" "{file_path}"'}
    ]
}

# =========================================================
# 2. Parsing Functions for Text Files Produced by Bash
# =========================================================
def parse_extensions(ext_file):
    app_exts = {}
    if not os.path.exists(ext_file):
        return app_exts
    
    with open(ext_file, 'r', encoding='utf-8') as f:
        current_app = None
        for line in f:
            line = line.strip()
            if line.startswith("App: "):
                current_app = line.replace("App: ", "").strip()
            elif line.startswith("Extensions: ") and current_app:
                exts_raw = line.replace("Extensions: ", "").strip()
                if exts_raw == "* (All Files)":
                    app_exts[current_app] = "['*']"
                else:
                    ext_list = re.split(r'[, ]+', exts_raw)
                    ext_list = [f"'{ext.lower()}'" for ext in ext_list if ext.startswith('.')]
                    app_exts[current_app] = "[" + ", ".join(ext_list) + "]"
    return app_exts

def parse_commands(cmd_file):
    apps = {}
    if not os.path.exists(cmd_file):
        return apps

    with open(cmd_file, 'r', encoding='utf-8') as f:
        current_app = None
        for line in f:
            line = line.strip()
            if line.startswith("App: "):
                current_app = line.replace("App: ", "").strip()
                if current_app not in apps:
                    apps[current_app] = {"exe_path": "", "actions": [], "is_com": False}
            elif line.startswith("ExePath: ") and current_app:
                path = line.replace("ExePath: ", "").replace(" (Guessed)", "").strip()
                path = re.sub(r'^[a-zA-Z]:', '~/.wine/drive_c', path)
                path = path.replace('\\', '/')
                apps[current_app]["exe_path"] = path
            elif line.startswith("Action: ") and current_app:
                action_name = line.replace("Action: ", "").strip()
                apps[current_app]["actions"].append({"name": action_name, "cmd": ""})
            elif line.startswith("Command: ") and current_app:
                cmd = line.replace("Command: ", "").strip()
                if "COM Object" in cmd:
                    apps[current_app]["is_com"] = True
                apps[current_app]["actions"][-1]["cmd"] = cmd
                
    # --- الحل هنا: الاستنتاج التلقائي للمسار لو كان مفقوداً ---
    for app, data in apps.items():
        if not data["exe_path"]:
            data["exe_path"] = f"~/.wine/drive_c/Program Files/{app}/{app}.exe"
            
    return apps

# =========================================================
# 3. Nautilus Extension Generator
# =========================================================
def generate_nautilus_extension(app_name, app_data, extensions):
    out_dir = os.path.expanduser("~/.local/share/nautilus-python/extensions")
    os.makedirs(out_dir, exist_ok=True)
    
    app_lower = app_name.lower().replace(" ", "_").replace("-", "_")
    app_class = re.sub(r'[^a-zA-Z0-9]', '', app_name)
    out_file = os.path.join(out_dir, f"winemenu_{app_lower}.py")
    
    actions = app_data["actions"]
    
    py_code = f"""import os
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
        self.valid_exts = set({extensions})
        self.actions = {actions}

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

        main_item = Nautilus.MenuItem(name='WineMenu::{app_class}Main', label='WineMenu')
        main_submenu = Nautilus.Menu()
        main_item.set_submenu(main_submenu)

        app_item = Nautilus.MenuItem(name='WineMenu::{app_class}App', label='{app_name}')
        app_submenu = Nautilus.Menu()
        app_item.set_submenu(app_submenu)
        main_submenu.append_item(app_item)

        for idx, action in enumerate(self.actions):
            item = Nautilus.MenuItem(name=f'WineAction::{{idx}}', label=action['name'])
            item.connect('activate', self.execute_wine_app, files, action['cmd'])
            app_submenu.append_item(item)

        return [main_item]

    def execute_wine_app(self, menu, files, raw_cmd):
        for file in files:
            linux_filepath = file.get_location().get_path()
            if not linux_filepath:
                continue
            
            try:
                win_filepath = subprocess.check_output(['winepath', '-w', linux_filepath]).decode('utf-8').strip()
            except subprocess.CalledProcessError:
                win_filepath = linux_filepath

            cmd_string = raw_cmd.replace('%1', win_filepath).replace('"%1"', f'"{{win_filepath}}"')
            parsed_cmd = shlex.split(cmd_string)
            
            if parsed_cmd[0].lower() != 'wine':
                final_cmd = ['wine'] + parsed_cmd
            else:
                final_cmd = parsed_cmd
                
            final_cmd[1] = os.path.expanduser(final_cmd[1])

            working_dir = os.path.dirname(linux_filepath)
            subprocess.Popen(final_cmd, cwd=working_dir)
"""
    
    with open(out_file, 'w', encoding='utf-8') as f:
        f.write(py_code)

# =========================================================
# Main Logic
# =========================================================
if __name__ == "__main__":
    print("--- [ Generating WineMenu Extensions for Nautilus ] ---")
    
    CMD_TARGET_FILE = "extracted_commands.txt"
    ext_data = parse_extensions("extracted_extensions.txt")
    cmd_data = parse_commands(CMD_TARGET_FILE)
    
    if not cmd_data:
        print("[!] No extracted commands found. Run compare_reg.sh first.")
        exit(1)
        
    for app, data in cmd_data.items():
        exts = ext_data.get(app, "['*']")
        
        if data["is_com"]:
            if app in COM_KNOWLEDGE_BASE:
                print(f"[*] COM Object Detected for {app}: Injecting known commands...")
                resolved_actions = []
                for action in COM_KNOWLEDGE_BASE[app]:
                    resolved_cmd = action["cmd"].replace("{exe_path}", data["exe_path"]).replace("{file_path}", "%1")
                    resolved_actions.append({"name": action["name"], "cmd": resolved_cmd})
                data["actions"] = resolved_actions
            else:
                print(f"[!] Warning: {app} is a COM Object but not in KNOWLEDGE_BASE. Using default open.")
                data["actions"] = [{"name": f"Open with {app}", "cmd": f'"{data["exe_path"]}" "%1"'}]
        
        generate_nautilus_extension(app, data, exts)

    # تحديث وتصدير الأوامر
    with open(CMD_TARGET_FILE, "w", encoding="utf-8") as f:
        for app, data in cmd_data.items():
            f.write("===================================\n")
            f.write(f"App: {app}\n")
            f.write(f"ExePath: {data['exe_path']}\n")
            for action in data["actions"]:
                f.write(f"Action: {action['name']}\n")
                f.write(f"Command: {action['cmd']}\n")
            f.write("\n")
            
    print(f"[+] Updated database with final commands in: {CMD_TARGET_FILE}")
    print("[+] Operation Complete! Restart Nautilus using: nautilus -q")