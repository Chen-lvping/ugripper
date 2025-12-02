# python starter

由 uv 管理的 Python 项目模板.

## 功能

- 快速依赖安装与管理 (uv)
- 类型提示与严格类型检查 (mypy)
- 代码 linting & formatting (ruff)
- 单元测试 (pytest)
- PEP 621  compliant project structure

## 环境要求

- Python >= 3.10
- uv >= 0.3.0 (安装方式 `curl -LsSf https://astral.sh/uv/install.sh | sh`)
- mypy >= 0.991 (安装方式 `uv add mypy`)
- ruff >= 0.0.280 (安装方式 `uv tool install ruff`,安装vscode ruff插件，或用 `uv tool run ruff` 来运行,这样所有环境都能使用ruff工具来格式化代码,而不只是当前的venv)
   You can configure Ruff to format Python code on-save by enabling the editor.formatOnSave action in settings.json, and setting Ruff as your default formatter:
   {
      "[python]": {
         "editor.formatOnSave": true,
         "editor.defaultFormatter": "charliermarsh.ruff"
      }
   }

- pytest >= 9.0.1 (安装方式 `uv add pytest`)

## 使用方式

1. 新的项目**直接 fork 本项目**, 并将项目名**替换**为你的**项目名**即可.
2. ❗使用特定版本的python来初始化项目

   ```bash
   sudo apt install pipx  # 系统python有保护，尝试用pipx安装uv
   pipx ensurepath
   pipx install uv
   uv init . --python 3.10 --name my_project
   ```
   > 注意: 如果 uv 命令不可用, 请确保 `~/.local/bin` 在你的 PATH 中:
   ```bash
   echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc # 可能bin不在path，如果不在的话，添加bin目录到PATH
   source ~/.bashrc
   ```

   换源
   ```bash
   echo 'export UV_DEFAULT_INDEX="https://mirrors.aliyun.com/pypi/simple"' >> ~/.bashrc
   source ~/.bashrc
   ```

3. ❗创建虚拟环境
   也可以使用pyproject.toml还原别人的项目环境，直接uv sync即可.
   ```bash
   uv venv
   ```

4. ❗激活虚拟环境

   ```bash
   source .venv/bin/activate
   ```
   可以在vscode项目配置里自动激活虚拟环境.
   项目根目录下创建 `.vscode/settings.json` 文件, 添加内容如下:

   ```json
   {
      "terminal.integrated.profiles.linux": {
         "UV Terminal": {
         "path": "/bin/bash",
         "args": ["--rcfile", "${workspaceFolder}/.vscode/uv_terminal.sh"]
         }
      },
      "terminal.integrated.defaultProfile.linux": "UV Terminal",
   }
   ```
   还需要
   创建 `.vscode/uv_terminal.sh` 文件并添加执行权限 sudo chmod +x, 根据实际路径修改 `<my_project>` 为你的项目相对工作空间的路径, 添加内容如下:

   ```bash
   #!/bin/bash
   # 载入原有 bash 配置（颜色、prompt 等）
   if [ -f "$HOME/.bashrc" ]; then
      source "$HOME/.bashrc"
   fi

   # 激活 UV 虚拟环境
   source "${PWD}/ugripper/.venv/bin/activate"
   ```

## 添加依赖

不推荐再用 pip 来添加依赖, 而应该用 uv 来添加依赖.使用 uv add 来添加依赖可以把依赖添加到 `pyproject.toml` 中, 并自动更新 `uv.lock` 文件, 确保依赖的版本一致.如果依赖已经在 `pyproject.toml` 中, 则会更新其版本.
在source venv 后, 才能使用 uv add 来添加依赖, 否则会报错.

   ```bash
   uv add <dependency>
   ```

比如添加 `pytest` 依赖

   ```bash
   uv add pytest
   ```

指定依赖版本

   ```bash
   uv add pytest==9.0.1
   ```

也可以指定依赖的最小版本

   ```bash
   uv add pytest>=9.0.1
   ```

## 移除依赖

   ```bash
   uv remove <dependency>
   ```

比如移除 `pytest` 依赖

   ```bash
   uv remove pytest
   ```  

## 同步依赖

   ```bash
   uv sync
   ```  

   根据 `pyproject.toml` 中的依赖, 同步 `uv.lock` 文件中的依赖,并在venv中安装依赖.


   ## 使用
   1. 运行 `uv sync` 来同步依赖.
   2. 运行 `uv run mypy` 来检查类型错误.
   3. 运行 `uv tool run ruff` 来格式化代码.
   4. 运行 `uv run pytest` 来运行测试.
   5. 运行 `uv run <your_script>` 来运行你的脚本.
   6. 运行 `python -m <project_name>` 会直接运行src中的 `__main__.py` 文件.
