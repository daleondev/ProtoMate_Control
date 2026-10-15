#!/usr/bin/env python3
"""Record conservative design allowances; these are not bench measurements."""
import json
from design import ROOT, REVISION

def threshold(top,bottom,reference):
    return reference*(1+top/bottom)

def main():
    # TPS26600 threshold tolerances plus independent 1% divider resistors.
    uv=[threshold(143000*.99,10000*1.01,1.175),threshold(143000,10000,1.19),threshold(143000*1.01,10000*.99,1.225)]
    ov=[threshold(226000*.99,10000*1.01,1.17),threshold(226000,10000,1.19),threshold(226000*1.01,10000*.99,1.225)]
    current=12000/8060
    # Design allowance: 80% conversion efficiency at the low operating rail.
    display_input=3.3*2/(18*.8)
    combined_input=(12*1+3.3*2)/(18*.8)
    # 100mm FFC with >=0.0105mm² copper, two parallel feeds, hot copper factor.
    cable_feed_ohm=.0175*.1/.0105/2*1.3
    cable_drop=display_input*(cable_feed_ohm+.1) # also allow return/contact losses
    # Display 3.3V nominal: accuracy 2%, line .5%, load 1%, temp .02%/K.
    # 0..70C is a calculation envelope; full-load thermal derating is tested.
    regulator_min=3.3*(1-.02-.005-.01-.0002*45)
    ripple_allowance=.05  # 100mVpp qualification allowance, not guaranteed spec
    copper_drop=1.5*.04
    contact_drop=.03
    panel_min=regulator_min-ripple_allowance-copper_drop-contact_drop
    assert ov[2]<36 and combined_input<current*.95
    assert display_input/2<.5 and panel_min>3.0
    result={
        'revision':REVISION,'status':'CALCULATED DESIGN BUDGET; PHYSICAL QUALIFICATION PENDING',
        'input':'24V nominal, recommended regulated 24V +/-10%, >=2A available for this assembly',
        'uv_start_min_typ_max_V':uv,'ov_cutoff_min_typ_max_V':ov,
        'efuse_typical_current_limit_A':current,'efuse_typical_24V_ramp_ms':24*47e-9/(24.6*4.7e-6)*1000,
        'both_converters_full_rated_output_input_A_at_18V_80percent':combined_input,
        'display_2A_input_A_at_18V_80percent':display_input,
        'FFC_power_current_per_contact_A':display_input/2,
        'FFC_24V_drop_allowance_at_full_module_load_V':cable_drop,
        'display_qualification_current_budget_A':1.5,
        'regulator_min_before_ripple_and_copper_V':regulator_min,
        'ripple_negative_allowance_V':ripple_allowance,
        'pcb_supply_return_allowance_ohm':.04,'connector_drop_allowance_V':contact_drop,
        'panel_min_calculated_V':panel_min,
        'panel_min_required_V':3.0,'panel_max_absolute_V':3.6,
        'limitations':[
            'Efficiency is a budgeting allowance; verify both modules at the actual 24V input, load and enclosure temperature.',
            'The panel backlight maximum is unspecified. Its four 0.3A contacts limit permissible backlight current; measure it.',
            '3.3V rise time must be measured at CN1 and satisfy the panel 1..20ms requirement.',
            'Converter ripple, hot copper/contact losses, current sharing and supply sequencing remain first-board tests.',
            'TVS/eFuse ratings do not establish a tested surge-immunity level.',
        ]}
    (ROOT/'manufacturing/verification/power-budget.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
