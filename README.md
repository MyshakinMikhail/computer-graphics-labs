# Лабораторные работы по компьютерной графике

Vulkan, GLFW, Dear ImGui, C++20. **Вариант 2 — правильный октаэдр.**
Основа проекта: [vladeemerr/vulkan-starter-app](https://github.com/vladeemerr/vulkan-starter-app).

| Работа | Содержание | Документация |
| --- | --- | --- |
| №1 | Два октаэдра, проекции, преобразования и анимация | [labs/lab1](labs/lab1/README.md) |
| №2 | Камера, материалы Блинна–Фонга, направленный свет, динамические массивы точечных источников и прожекторов, визуализация источников | [labs/lab2](labs/lab2/README.md) |

![Лабораторная №2](labs/lab2/docs/preview.png)

## Сборка на macOS

Нужны CMake, Ninja, компилятор C++20 и [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) с MoltenVK и `glslc`.
Команды выполняются из корня этого репозитория:

```bash
brew install cmake ninja
source "$HOME/VulkanSDK/1.4.321.0/setup-env.sh" # укажите установленную версию SDK
cmake --preset debug
cmake --build build-debug --parallel
./build-debug/lab2
```

Первая конфигурация скачивает зависимости с GitHub. Шейдеры автоматически компилируются в `build-debug/shaders/lab1` и `build-debug/shaders/lab2`. Путь к ним задаётся при сборке: запускать программы можно из любой рабочей директории. При переносе репозитория в другую папку пересоздайте каталог сборки.

Обе лабораторные собираются независимо:

```bash
cmake --build build-debug --target lab1 --parallel
./build-debug/lab1
cmake --build build-debug --target lab2 --parallel
./build-debug/lab2
```

На Linux и Windows используется тот же CMake-проект. Установите Vulkan SDK и платформенные зависимости GLFW; доступны также пресеты `release`, `msvc-debug`, `msvc-release`, `mingw-debug`, `mingw-release`. Выполнявшаяся здесь проверка графики — macOS/MoltenVK.

## Структура

```text
common/                 # GLFW/ImGui main loop и общий Vulkan backend
labs/lab1/source/       # исходная лабораторная №1
labs/lab1/shaders/
labs/lab2/source/       # камера, геометрия, состояние сцены, интерфейс, renderer
labs/lab2/shaders/      # освещение и отдельная отрисовка обозначений источников
labs/lab2/tests/        # проверки геометрии, камеры, математики и GPU layout
```

Каждая лабораторная имеет свою исполняемую цель, шейдеры, README и настройки ImGui. Лабораторная №1 сохраняет исходные возможности; общий код запуска используется обеими работами.

## Проверка

```bash
ctest --test-dir build-debug --output-on-failure
./build-debug/lab1 --frames 15
./build-debug/lab2 --exercise
```

`--exercise` автоматически завершает работу после 110 кадров. Проверяет пустые массивы, рост обоих storage-буферов до 37 источников, удаление источников, равные углы прожектора, нулевые направления, неравномерный масштаб, анимацию и изменение размера окна. `--frames N` ограничивает длительность обычного запуска.

Для проверки синхронизации с установленным validation layer:

```bash
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT ./build-debug/lab2 --exercise
```

Ошибки Vulkan validation выводятся в консоль и приводят к ненулевому коду завершения. Подробный сценарий проверки и соответствие заданию — в [README второй лабораторной](labs/lab2/README.md).
