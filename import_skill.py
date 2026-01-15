import sqlite3
import json
import sys

def import_skill(db_path, skill_file):
    try:
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()

        with open(skill_file, 'r') as f:
            name = None
            description = None
            patch = None
            for line in f:
                line = line.strip()
                if line.startswith('#name:'):
                    name = line[6:].strip()
                elif line.startswith('#description:'):
                    description = line[13:].strip()
                elif line.startswith('#patch:'):
                    patch = line[7:].strip()

            if not all([name, description, patch]):
                raise ValueError("Skill file must contain #name, #description, and #patch fields.")

            sql = "INSERT OR IGNORE INTO skills (name, description, system_prompt_patch) VALUES (?, ?, ?)"
            cursor.execute(sql, (name, description, patch))
            conn.commit()

        print(f"Skill '{name}' imported successfully.")

    except sqlite3.Error as e:
        print(f"SQLite error: {e}")
    except FileNotFoundError:
        print(f"Error: Skill file '{skill_file}' not found.")
    except ValueError as e:
        print(f"Error: {e}")
    finally:
        if conn:
            conn.close()

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python import_skill.py <skill_file>")
        sys.exit(1)

    db_path = "slop.db"
    skill_file = sys.argv[1]
    import_skill(db_path, skill_file)
