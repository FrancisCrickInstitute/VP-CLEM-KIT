# Project Setup

This project requires several packages. The dependencies are defined in three files:

- **requirements.txt**: Contains the primary pip installation requirements.
- **environment.yml**: Provides instructions for setting up a Conda environment.
- **stable_packages.txt**: Lists stable versions of all packages, which you should use if you run into any versioning issues.

## Installation Instructions

### Using Conda

If you use Conda, you can create a dedicated environment with all required packages:

1. Open your terminal.
2. Run the following command to create the environment:

   ```bash
   conda env create -f environment.yml

3. Activate the environment

   ```bash
   conda activate localize_render_env

This setup ensures that all package dependencies are met as specified in the environment file.

### Using Pip

If you prefer using pip, follow these steps:

1. (Optional) Create a virtual environment to keep dependencies isolated:

   ```bash  
    python -m venv myenv
    ```

Activate it:

   -  On macOS/Linux:

   ```bash
   source myenv/bin/activate 
   ```

   - On Windows:

   ```bash
   myenv\Scripts\activate
   ```
2. Install the required packages by running:

   ```
    pip install -r requirements.txt
    ```

### Troubleshooting Version Issues

If you encounter any issues with package versions or compatibility, please install the stable versions of the packages. These versions are listed in the stable_packages.txt file. To install the stable versions, run:

```
pip install -r stable_packages.txt
```

Using the stable versions should resolve most version-related issues.

## Running the Jupyter Notebooks

After installing the required packages, you can run the Jupyter notebooks as follows:

1. **Activate your environment** (if not already active) using the instructions above.
2. **Launch JupyterLab** by running one of the following commands:

   ```bash
   jupyter lab

3. **Navigate to the project directory** in the Jupyter interface, where you'll find the notebooks (e.g., `filter_render_Win.ipynb` and `filter_render_macOS.ipynb`).
4. **Open and run the notebook** you want to work with.
