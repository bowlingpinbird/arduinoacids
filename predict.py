import sys
from PeptideBuilder import Geometry
import PeptideBuilder
import Bio.PDB

# Standard 20 one-letter amino acid codes supported by PeptideBuilder
VALID_AA = set("ACDEFGHIKLMNPQRSTVWY")

def build_peptide(sequence: str, output_file: str = "built_peptide.pdb"):
    # Clean up whitespace and convert to uppercase
    seq = sequence.strip().upper()
    
    if not seq:
        raise ValueError("No amino acid sequence provided.")
        
    # Check for any invalid letters
    invalid = [aa for aa in seq if aa not in VALID_AA]
    if invalid:
        raise ValueError(f"Invalid amino acid code(s): {', '.join(invalid)}")

    # 1. Initialize structure with the FIRST amino acid (seq[0])
    geo = Geometry.geometry(seq[0])
    structure = PeptideBuilder.initialize_res(geo)

    # 2. Add the REMAINING residues (seq[1:])
    for aa in seq[1:]:
        structure = PeptideBuilder.add_residue(structure, aa)

    # 3. Save to PDB
    out = Bio.PDB.PDBIO()
    out.set_structure(structure)
    out.save(output_file)
    print(f"Successfully built {output_file} for sequence: {seq}")

if __name__ == "__main__":
    # Get sequence from command-line argument passed by app.py, or fallback to a default
    input_seq = sys.argv[1] if len(sys.argv) > 1 else "ACDEFG"
    build_peptide(input_seq)
