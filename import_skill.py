import sqlite3
import json
import os

db_path = 'slop.db'
json_path = 'skill_default.json'

with open(json_path, 'r') as f:
    data = json.load(f)

conn = sqlite3.connect(db_path)
cursor = conn.cursor()

# We want to insert it as 'assistant_manager'
name = 'assistant_manager'
description = data.get('description', '')
system_prompt_patch = data.get('system_prompt_patch', '')

try:
    cursor.execute("INSERT INTO skills (name, description, system_prompt_patch) VALUES (?, ?, ?)", 
                   (name, description, system_prompt_patch))
    conn.commit()
    print(f"Successfully imported skill '{name}' into {db_path}")
except sqlite3.IntegrityError:
    print(f"Skill '{name}' already exists in {db_path}")
except Exception as e:
    print(f"Error: {e}")
finally:
    conn.close()
