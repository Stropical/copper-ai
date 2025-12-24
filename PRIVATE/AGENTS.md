com_copperai_copperagent is a KiCad plugin, this is required to be GPLv3. It should contain a ton of tools that are accesible by pcb_agent

pcb_agent is a TRUE agentic tool that operates like cursor but for PCBs

router_server tracks token spend and logs to supabase

website is the front landing page for copper ai


CopperAI should be able to:

Take an idea and convert it into a schematic
    Read a datasheet for a chip, and then build a list of requirements for how each pin should be connected
    Understand the inputs and outputs of a current block in a block diagram
Make a schematic capable of running in SPICE
    Use SPICE components, place them in a circuit
    Wire parts together in a not messy way
    Simulate it in spice, and analyze the output
Auto-place componenets given the edge cuts of a PCB
Dynamically route components based on their requirements (high frequency, different amperages, cross-talk, etc)
Analyze DRC errors and be able to fix
